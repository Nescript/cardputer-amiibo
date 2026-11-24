#include "usb_switch_hid.h"

#include <Arduino.h>
#include <esp_err.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/periph_ctrl.h>
#include <soc/periph_defs.h>

#include <algorithm>
#include <cstring>

extern "C" {
#include "device/usbd_pvt.h"
#include "esp32-hal-tinyusb.h"
#include "pa_device.h"
#include "pa_identity.h"
#include "ns2_vendor_rx.h"
#include "ns2_vendor_tx.h"
}

// esp32-hal-tinyusb.c keeps this one-field type private, while exporting the
// installer that initializes the USB HAL without creating Arduino's unpinned
// `usbd` task. Keep this declaration ABI-matched to that implementation.
struct Switch2TinyUsbHalConfig {
    bool external_phy;
};
extern "C" esp_err_t tinyusb_driver_install(const Switch2TinyUsbHalConfig* config);

namespace amiibo {
namespace {

constexpr uint8_t kHidInterface = 0;
constexpr uint8_t kVendorInterface = 1;
constexpr uint8_t kHidEpIn = 0x81;
constexpr uint8_t kHidEpOut = 0x01;
constexpr uint8_t kVendorEpOut = 0x02;
constexpr uint8_t kVendorEpIn = 0x82;
constexpr uint16_t kPacketSize = 64;
constexpr size_t kResponseQueueCapacity = 8;
constexpr uint8_t kReportIdGamepad = 0x09;
constexpr uint8_t kReportIdImportOut = 0x02;
constexpr uint8_t kReportIdImportIn = 0x05;
constexpr uint8_t kImportMarker[] = {0xA5, 0x41, 0x4D, 0x42, 0x31};
constexpr uint16_t kHidReportDescriptorSize = 97;
constexpr uint32_t kTagRemovalMs = 150;

enum class VendorInStage : uint8_t {
    Idle,
    Data,
    NeedZlp,
    Zlp,
};

struct QueuedResponse {
    uint8_t data[128]{};
    uint16_t size{0};
};

struct DriverState {
    SemaphoreHandle_t state_mutex{nullptr};
    SemaphoreHandle_t init_done{nullptr};
    TaskHandle_t task{nullptr};

    pa_device_t device{};
    ns2_vendor_rx_t vendor_rx{};
    ns2_vendor_tx_t vendor_tx{};
    ControllerState controller{};

    QueuedResponse response_queue[kResponseQueueCapacity]{};
    size_t response_head{0};
    size_t response_tail{0};
    size_t response_count{0};
    UsbImportReport import_report{};
    UsbImportReport import_reply{};

    uint8_t hid_interface{kHidInterface};
    uint8_t vendor_interface{kVendorInterface};
    uint8_t hid_ep_in{0};
    uint8_t hid_ep_out{0};
    uint8_t vendor_ep_out{0};
    uint8_t vendor_ep_in{0};
    uint8_t hid_protocol{1};
    uint8_t hid_idle{0};

    uint32_t rx_packets{0};
    uint32_t tx_packets{0};
    uint32_t nfc_poll_count{0};
    uint8_t last_out_report{0};
    uint8_t last_subcmd{0};
    uint32_t last_hid_report_ms{0};
    uint32_t rotation_started_ms{0};

    VendorInStage vendor_in_stage{VendorInStage::Idle};
    bool begin_attempted{false};
    bool begin_result{false};
    bool mounted{false};
    bool selected_tag_present{false};
    bool auto_tag_rotation{false};
    bool rotation_pending{false};
    bool hid_out_armed{false};
    bool vendor_out_armed{false};
    bool vendor_zlp_required{false};
    bool import_enabled{false};
    bool import_pending{false};
    bool import_reply_pending{false};
    bool import_reply_in_flight{false};
};

DriverState g_driver;

CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t g_hid_out_buffer[kPacketSize];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t g_hid_in_buffer[kPacketSize];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t g_vendor_out_buffer[kPacketSize];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t g_vendor_in_buffer[NS2_VENDOR_TX_MAX_RESPONSE];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t g_control_buffer[kPacketSize];

constexpr uint8_t kHidDescriptor[9] = {
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x61, 0x00,
};

bool takeState(TickType_t timeout = pdMS_TO_TICKS(10)) {
    return g_driver.state_mutex &&
           xSemaphoreTake(g_driver.state_mutex, timeout) == pdTRUE;
}

void giveState() {
    xSemaphoreGive(g_driver.state_mutex);
}

bool rotationReady(uint32_t now) {
    return g_driver.rotation_pending &&
           static_cast<uint32_t>(now - g_driver.rotation_started_ms) >= kTagRemovalMs;
}

void resetNfcSession() {
    g_driver.rotation_pending = false;
    pa_device_reset_nfc(&g_driver.device);
    pa_device_present(&g_driver.device, g_driver.selected_tag_present);
}

void resetResponseQueue() {
    g_driver.response_head = 0;
    g_driver.response_tail = 0;
    g_driver.response_count = 0;
}

bool queueResponse(const uint8_t* data, size_t size) {
    if (!data || size == 0 || size > sizeof(g_driver.response_queue[0].data) ||
        g_driver.response_count >= kResponseQueueCapacity)
        return false;

    QueuedResponse& slot = g_driver.response_queue[g_driver.response_tail];
    std::memcpy(slot.data, data, size);
    slot.size = static_cast<uint16_t>(size);
    g_driver.response_tail = (g_driver.response_tail + 1) % kResponseQueueCapacity;
    ++g_driver.response_count;
    return true;
}

bool popResponse(QueuedResponse& out) {
    if (g_driver.response_count == 0) return false;
    out = g_driver.response_queue[g_driver.response_head];
    g_driver.response_head = (g_driver.response_head + 1) % kResponseQueueCapacity;
    --g_driver.response_count;
    return true;
}

void resetSessionState() {
    if (takeState()) {
        g_driver.mounted = false;
        g_driver.hid_out_armed = false;
        g_driver.vendor_out_armed = false;
        g_driver.vendor_in_stage = VendorInStage::Idle;
        g_driver.vendor_zlp_required = false;
        g_driver.import_pending = false;
        g_driver.import_reply_pending = false;
        g_driver.import_reply_in_flight = false;
        g_driver.import_report.fill(0);
        g_driver.import_reply.fill(0);
        g_driver.hid_ep_in = 0;
        g_driver.hid_ep_out = 0;
        g_driver.vendor_ep_out = 0;
        g_driver.vendor_ep_in = 0;
        g_driver.rx_packets = 0;
        g_driver.tx_packets = 0;
        g_driver.nfc_poll_count = 0;
        g_driver.last_out_report = 0;
        g_driver.last_subcmd = 0;
        resetNfcSession();
        pa_identity_init();
        giveState();
    }

    ns2_vendor_rx_init(&g_driver.vendor_rx);
    ns2_vendor_tx_init(&g_driver.vendor_tx);
    resetResponseQueue();
}

void armHidOut() {
    if (!g_driver.mounted || !g_driver.hid_ep_out || g_driver.hid_out_armed ||
        usbd_edpt_busy(0, g_driver.hid_ep_out))
        return;

    if (usbd_edpt_xfer(0, g_driver.hid_ep_out, g_hid_out_buffer,
                       sizeof(g_hid_out_buffer)))
        g_driver.hid_out_armed = true;
}

void acceptImportReport(const uint8_t* report, size_t size) {
    if (!report || size != kPacketSize || report[0] != kReportIdImportOut ||
        std::memcmp(report + 1, kImportMarker, sizeof(kImportMarker)) != 0 ||
        !takeState(pdMS_TO_TICKS(2)))
        return;

    if (g_driver.mounted && g_driver.import_enabled &&
        !g_driver.import_pending && !g_driver.import_reply_pending &&
        !g_driver.import_reply_in_flight) {
        std::memcpy(g_driver.import_report.data(), report, kPacketSize);
        g_driver.import_report[0] = 0x80;
        g_driver.import_pending = true;
    }
    giveState();
}

void serviceImportReply() {
    if (!g_driver.mounted || !g_driver.hid_ep_in ||
        usbd_edpt_busy(0, g_driver.hid_ep_in))
        return;

    UsbImportReport reply{};
    if (!takeState(pdMS_TO_TICKS(2))) return;
    const bool pending = g_driver.import_enabled && g_driver.import_reply_pending &&
                         !g_driver.import_reply_in_flight;
    if (pending) {
        reply = g_driver.import_reply;
        g_driver.import_reply_pending = false;
        g_driver.import_reply_in_flight = true;
    }
    giveState();
    if (!pending) return;

    if (!usbd_edpt_claim(0, g_driver.hid_ep_in)) {
        if (takeState(pdMS_TO_TICKS(2))) {
            g_driver.import_reply_in_flight = false;
            g_driver.import_reply_pending = true;
            giveState();
        }
        return;
    }

    g_hid_in_buffer[0] = kReportIdImportIn;
    std::memcpy(g_hid_in_buffer + 1, reply.data() + 1, kPacketSize - 1);
    if (!usbd_edpt_xfer(0, g_driver.hid_ep_in, g_hid_in_buffer, kPacketSize)) {
        usbd_edpt_release(0, g_driver.hid_ep_in);
        if (takeState(pdMS_TO_TICKS(2))) {
            g_driver.import_reply_in_flight = false;
            g_driver.import_reply_pending = true;
            giveState();
        }
    }
}

bool hasPendingVendorReply() {
    return g_driver.response_count != 0 || g_driver.vendor_tx.active ||
           g_driver.vendor_in_stage != VendorInStage::Idle;
}

void armVendorOut() {
    if (!g_driver.mounted || !g_driver.vendor_ep_out || g_driver.vendor_out_armed ||
        hasPendingVendorReply() || usbd_edpt_busy(0, g_driver.vendor_ep_out))
        return;

    if (usbd_edpt_xfer(0, g_driver.vendor_ep_out, g_vendor_out_buffer,
                       sizeof(g_vendor_out_buffer)))
        g_driver.vendor_out_armed = true;
}

void dispatchVendorCommand(void*, const uint8_t* command, size_t size) {
    if (!command || size == 0 || !takeState(pdMS_TO_TICKS(20))) return;

    ++g_driver.rx_packets;
    g_driver.last_out_report = command[0];
    g_driver.last_subcmd = size > 3 ? command[3] : 0;
    if (command[0] == 0x01 && size > 3 &&
        (command[3] == 0x03 || command[3] == 0x04 || command[3] == 0x05))
        ++g_driver.nfc_poll_count;

    uint8_t response[128]{};
    const uint32_t now = millis();
    const uint32_t previous_completions = g_driver.device.completed_reads;
    const size_t response_size = pa_device_command(
        &g_driver.device, now, command, size, response);
    if (response_size != 0 && response_size <= sizeof(response) &&
        queueResponse(response, response_size) &&
        g_driver.auto_tag_rotation && g_driver.selected_tag_present &&
        previous_completions != g_driver.device.completed_reads) {
        pa_device_present(&g_driver.device, false);
        g_driver.rotation_pending = true;
        g_driver.rotation_started_ms = now;
    }

    giveState();
}

void handleVendorOut(uint32_t transferred) {
    g_driver.vendor_out_armed = false;
    if (transferred > 0 && transferred <= sizeof(g_vendor_out_buffer)) {
        ns2_vendor_rx_feed(&g_driver.vendor_rx, g_vendor_out_buffer, transferred,
                           dispatchVendorCommand, nullptr);
    }
    armVendorOut();
}

size_t writeVendorIn(void*, const uint8_t* data, size_t size) {
    if (!data || size == 0 || size > sizeof(g_vendor_in_buffer) ||
        !g_driver.mounted || !g_driver.vendor_ep_in ||
        g_driver.vendor_in_stage != VendorInStage::Idle)
        return 0;

    if (!usbd_edpt_claim(0, g_driver.vendor_ep_in)) return 0;
    std::memcpy(g_vendor_in_buffer, data, size);
    if (!usbd_edpt_xfer(0, g_driver.vendor_ep_in, g_vendor_in_buffer,
                        static_cast<uint16_t>(size))) {
        usbd_edpt_release(0, g_driver.vendor_ep_in);
        return 0;
    }

    g_driver.vendor_zlp_required = (size % kPacketSize) == 0;
    g_driver.vendor_in_stage = VendorInStage::Data;
    return size;
}

bool startVendorZlp() {
    if (g_driver.vendor_in_stage != VendorInStage::NeedZlp ||
        !g_driver.vendor_ep_in || !usbd_edpt_claim(0, g_driver.vendor_ep_in))
        return false;

    if (!usbd_edpt_xfer(0, g_driver.vendor_ep_in, g_vendor_in_buffer, 0)) {
        usbd_edpt_release(0, g_driver.vendor_ep_in);
        return false;
    }

    g_driver.vendor_in_stage = VendorInStage::Zlp;
    return true;
}

void serviceVendorIn() {
    if (g_driver.vendor_in_stage == VendorInStage::NeedZlp) {
        (void)startVendorZlp();
        return;
    }
    if (g_driver.vendor_in_stage != VendorInStage::Idle || !g_driver.mounted)
        return;

    if (!g_driver.vendor_tx.active && g_driver.response_count != 0) {
        QueuedResponse next{};
        if (popResponse(next))
            (void)ns2_vendor_tx_queue(&g_driver.vendor_tx, next.data, next.size);
    }

    if (g_driver.vendor_tx.active)
        ns2_vendor_tx_pump(&g_driver.vendor_tx, writeVendorIn, nullptr);

    armVendorOut();
}

void packButtons(const ControllerState& state, uint8_t buttons[3]) {
    buttons[0] = 0;
    buttons[1] = 0;
    buttons[2] = 0;

    if (state.b) buttons[0] |= 0x01;
    if (state.a) buttons[0] |= 0x02;
    if (state.y) buttons[0] |= 0x04;
    if (state.x) buttons[0] |= 0x08;
    if (state.r) buttons[0] |= 0x10;
    if (state.zr) buttons[0] |= 0x20;
    if (state.plus) buttons[0] |= 0x40;
    if (state.r_stick) buttons[0] |= 0x80;

    if (state.dpad_down) buttons[1] |= 0x01;
    if (state.dpad_right) buttons[1] |= 0x02;
    if (state.dpad_left) buttons[1] |= 0x04;
    if (state.dpad_up) buttons[1] |= 0x08;
    if (state.l) buttons[1] |= 0x10;
    if (state.zl) buttons[1] |= 0x20;
    if (state.minus) buttons[1] |= 0x40;
    if (state.l_stick) buttons[1] |= 0x80;

    if (state.home) buttons[2] |= 0x01;
    if (state.capture) buttons[2] |= 0x02;
    if (state.l_sr || state.l_sl) buttons[2] |= 0x08;
    if (state.r_sr || state.r_sl) buttons[2] |= 0x04;
}

void sendGamepadReport() {
    if (!g_driver.mounted || !g_driver.hid_ep_in ||
        !pa_identity_streaming() || pa_identity_report_id() != kReportIdGamepad ||
        usbd_edpt_busy(0, g_driver.hid_ep_in))
        return;

    const uint32_t now = millis();
    if (static_cast<uint32_t>(now - g_driver.last_hid_report_ms) < 1u ||
        !usbd_edpt_claim(0, g_driver.hid_ep_in))
        return;

    uint8_t buttons[3]{};
    g_hid_in_buffer[0] = kReportIdGamepad;
    if (!takeState(pdMS_TO_TICKS(5))) {
        usbd_edpt_release(0, g_driver.hid_ep_in);
        return;
    }
    packButtons(g_driver.controller, buttons);
    pa_device_report(&g_driver.device, now, buttons, g_hid_in_buffer + 1);
    giveState();

    if (!usbd_edpt_xfer(0, g_driver.hid_ep_in, g_hid_in_buffer,
                        sizeof(g_hid_in_buffer))) {
        usbd_edpt_release(0, g_driver.hid_ep_in);
        return;
    }
    g_driver.last_hid_report_ms = now;
}

void drv_init() {}

void drv_reset(uint8_t) {
    resetSessionState();
}

uint16_t drv_open(uint8_t rhport, tusb_desc_interface_t const* desc_intf,
                  uint16_t max_len) {
    if (!desc_intf ||
        (desc_intf->bInterfaceClass != TUSB_CLASS_HID &&
         desc_intf->bInterfaceClass != 0xFF))
        return 0;

    const bool hid = desc_intf->bInterfaceClass == TUSB_CLASS_HID;
    if ((hid && desc_intf->bInterfaceNumber != kHidInterface) ||
        (!hid && desc_intf->bInterfaceNumber != kVendorInterface))
        return 0;

    uint16_t consumed = sizeof(tusb_desc_interface_t);
    uint8_t const* desc = tu_desc_next(reinterpret_cast<uint8_t const*>(desc_intf));
    uint8_t ep_in = 0;
    uint8_t ep_out = 0;
    uint8_t endpoints_opened = 0;

    while (endpoints_opened < desc_intf->bNumEndpoints) {
        if (consumed + 2 > max_len) return 0;
        const uint8_t desc_len = tu_desc_len(desc);
        if (desc_len < 2 || consumed + desc_len > max_len) return 0;

        if (tu_desc_type(desc) == TUSB_DESC_ENDPOINT) {
            auto const* endpoint = reinterpret_cast<tusb_desc_endpoint_t const*>(desc);
            if (!usbd_edpt_open(rhport, endpoint)) return 0;
            if (tu_edpt_dir(endpoint->bEndpointAddress) == TUSB_DIR_IN)
                ep_in = endpoint->bEndpointAddress;
            else
                ep_out = endpoint->bEndpointAddress;
            ++endpoints_opened;
        } else if (!(hid && tu_desc_type(desc) == 0x21)) {
            return 0;
        }

        consumed += desc_len;
        desc = tu_desc_next(desc);
    }

    if (!ep_in || !ep_out) return 0;
    if (hid) {
        if (ep_in != kHidEpIn || ep_out != kHidEpOut) return 0;
        g_driver.hid_interface = desc_intf->bInterfaceNumber;
        g_driver.hid_ep_in = ep_in;
        g_driver.hid_ep_out = ep_out;
    } else {
        if (ep_in != kVendorEpIn || ep_out != kVendorEpOut) return 0;
        g_driver.vendor_interface = desc_intf->bInterfaceNumber;
        g_driver.vendor_ep_in = ep_in;
        g_driver.vendor_ep_out = ep_out;
    }
    return consumed;
}

bool drv_control_xfer_cb(uint8_t rhport, uint8_t stage,
                         tusb_control_request_t const* request) {
    if (!request || request->wIndex != g_driver.hid_interface ||
        request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE)
        return false;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        if (request->bRequest != TUSB_REQ_GET_DESCRIPTOR) return false;

        const uint8_t descriptor_type = static_cast<uint8_t>(request->wValue >> 8);
        if (descriptor_type == 0x22) {
            return tud_control_xfer(
                rhport, request,
                const_cast<uint8_t*>(ns2_hid_report_descriptor()),
                kHidReportDescriptorSize);
        }
        if (descriptor_type == 0x21)
            return tud_control_xfer(rhport, request,
                                    const_cast<uint8_t*>(kHidDescriptor),
                                    sizeof(kHidDescriptor));
        return false;
    }

    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) return false;
    switch (request->bRequest) {
        case 0x01:  // GET_REPORT
            if (stage == CONTROL_STAGE_SETUP) {
                std::memset(g_control_buffer, 0, sizeof(g_control_buffer));
                g_control_buffer[0] = static_cast<uint8_t>(request->wValue);
                const uint16_t length = std::min<uint16_t>(request->wLength,
                                                           sizeof(g_control_buffer));
                return tud_control_xfer(rhport, request, g_control_buffer, length);
            }
            return true;
        case 0x02:  // GET_IDLE
            if (stage == CONTROL_STAGE_SETUP) {
                g_control_buffer[0] = g_driver.hid_idle;
                return tud_control_xfer(rhport, request, g_control_buffer, 1);
            }
            return true;
        case 0x03:  // GET_PROTOCOL
            if (stage == CONTROL_STAGE_SETUP) {
                g_control_buffer[0] = g_driver.hid_protocol;
                return tud_control_xfer(rhport, request, g_control_buffer, 1);
            }
            return true;
        case 0x09:  // SET_REPORT, accepted and ignored; no rumble hardware exists.
            if (stage == CONTROL_STAGE_SETUP) {
                if (request->wLength > sizeof(g_control_buffer)) return false;
                return tud_control_xfer(rhport, request, g_control_buffer,
                                        request->wLength);
            }
            if (stage == CONTROL_STAGE_ACK &&
                (request->wValue & 0xFFU) == kReportIdImportOut &&
                (request->wValue >> 8U) == 0x02U) {
                uint8_t report[kPacketSize]{};
                if (request->wLength == kPacketSize - 1) {
                    report[0] = kReportIdImportOut;
                    std::memcpy(report + 1, g_control_buffer, kPacketSize - 1);
                    acceptImportReport(report, sizeof(report));
                } else if (request->wLength == kPacketSize &&
                           g_control_buffer[0] == kReportIdImportOut) {
                    std::memcpy(report, g_control_buffer, sizeof(report));
                    acceptImportReport(report, sizeof(report));
                }
            }
            return true;
        case 0x0A:  // SET_IDLE
            if (stage == CONTROL_STAGE_SETUP) {
                g_driver.hid_idle = static_cast<uint8_t>(request->wValue >> 8);
                return tud_control_status(rhport, request);
            }
            return true;
        case 0x0B:  // SET_PROTOCOL
            if (stage == CONTROL_STAGE_SETUP) {
                g_driver.hid_protocol = static_cast<uint8_t>(request->wValue);
                return tud_control_status(rhport, request);
            }
            return true;
        default:
            return false;
    }
}

bool drv_xfer_cb(uint8_t, uint8_t ep_addr, xfer_result_t result,
                 uint32_t xferred_bytes) {
    if (ep_addr == g_driver.hid_ep_out) {
        g_driver.hid_out_armed = false;
        if (result == XFER_RESULT_SUCCESS)
            acceptImportReport(g_hid_out_buffer, xferred_bytes);
        armHidOut();
        return true;
    }

    if (ep_addr == g_driver.hid_ep_in) {
        if (takeState(pdMS_TO_TICKS(2))) {
            if (g_driver.import_reply_in_flight) {
                g_driver.import_reply_in_flight = false;
                if (result != XFER_RESULT_SUCCESS && g_driver.import_enabled &&
                    g_driver.mounted)
                    g_driver.import_reply_pending = true;
                else
                    g_driver.import_reply.fill(0);
            }
            if (result == XFER_RESULT_SUCCESS) ++g_driver.tx_packets;
            giveState();
        }
        return true;
    }

    if (ep_addr == g_driver.vendor_ep_out) {
        if (result == XFER_RESULT_SUCCESS)
            handleVendorOut(xferred_bytes);
        else {
            g_driver.vendor_out_armed = false;
            armVendorOut();
        }
        return true;
    }

    if (ep_addr == g_driver.vendor_ep_in) {
        if (result != XFER_RESULT_SUCCESS) {
            g_driver.vendor_in_stage = VendorInStage::Idle;
            g_driver.vendor_zlp_required = false;
            return true;
        }

        if (g_driver.vendor_in_stage == VendorInStage::Data) {
            if (g_driver.vendor_zlp_required)
                g_driver.vendor_in_stage = VendorInStage::NeedZlp;
            else {
                g_driver.vendor_in_stage = VendorInStage::Idle;
                g_driver.vendor_zlp_required = false;
                if (takeState(pdMS_TO_TICKS(2))) {
                    ++g_driver.tx_packets;
                    giveState();
                }
            }
        } else if (g_driver.vendor_in_stage == VendorInStage::Zlp) {
            g_driver.vendor_in_stage = VendorInStage::Idle;
            g_driver.vendor_zlp_required = false;
            if (takeState(pdMS_TO_TICKS(2))) {
                ++g_driver.tx_packets;
                giveState();
            }
        }
        return true;
    }

    return false;
}

const usbd_class_driver_t kSwitch2AppDriver = {
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    "Switch2",
#endif
    drv_init,
    drv_reset,
    drv_open,
    drv_control_xfer_cb,
    drv_xfer_cb,
    nullptr,
};

}  // namespace

SwitchUsbTransport& SwitchUsbTransport::instance() {
    static SwitchUsbTransport transport;
    return transport;
}

void SwitchUsbTransport::usbTxTaskEntry(void* arg) {
    static_cast<SwitchUsbTransport*>(arg)->usbTxTaskLoop();
}

void SwitchUsbTransport::usbTxTaskLoop() {
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_enable(PERIPH_USB_MODULE);
    const Switch2TinyUsbHalConfig config{false};
    g_driver.begin_result = tinyusb_driver_install(&config) == ESP_OK;
    xSemaphoreGive(g_driver.init_done);
    if (!g_driver.begin_result) {
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        // TinyUSB callbacks and protocol state changes execute on this pinned
        // Core 0 task. Core 1 only copies preloaded RAM state through the API.
        tud_task_ext(1, false);
        serviceVendorIn();
        serviceImportReply();
        armHidOut();
        armVendorOut();
        sendGamepadReport();
    }
}

bool SwitchUsbTransport::begin() {
    if (g_driver.begin_attempted) return g_driver.begin_result;
    g_driver.begin_attempted = true;

    g_driver.state_mutex = xSemaphoreCreateMutex();
    g_driver.init_done = xSemaphoreCreateBinary();
    if (!g_driver.state_mutex || !g_driver.init_done) return false;

    pa_device_init(&g_driver.device);
    ns2_vendor_rx_init(&g_driver.vendor_rx);
    ns2_vendor_tx_init(&g_driver.vendor_tx);
    if (xTaskCreatePinnedToCore(usbTxTaskEntry, "usb_tx_task", 6144, this, 5,
                                &g_driver.task, 0) != pdPASS)
        return false;

    if (xSemaphoreTake(g_driver.init_done, pdMS_TO_TICKS(5000)) != pdTRUE)
        return false;
    return g_driver.begin_result;
}

void SwitchUsbTransport::setControllerState(const ControllerState& state) {
    if (!takeState(pdMS_TO_TICKS(5))) return;
    g_driver.controller = state;
    pa_device_set_sticks(&g_driver.device, state.lx, state.ly, state.rx, state.ry);
    giveState();
}

void SwitchUsbTransport::loadAndArmTag(const AmiiboTag& tag, bool present) {
    if (!takeState(pdMS_TO_TICKS(20))) return;

    const bool requested_present = present && tag.isValid();
    if (!g_driver.rotation_pending && g_driver.device.loaded && tag.isValid() &&
        std::memcmp(g_driver.device.raw, tag.data(), AmiiboTag::size()) == 0 &&
        g_driver.selected_tag_present == requested_present) {
        giveState();
        return;
    }

    const bool loaded = pa_device_load_raw(&g_driver.device, tag.data(),
                                           AmiiboTag::size());
    g_driver.rotation_pending = false;
    g_driver.selected_tag_present = loaded && requested_present;
    pa_device_present(&g_driver.device, g_driver.selected_tag_present);
    giveState();
}

void SwitchUsbTransport::setTagPresent(bool present) {
    if (!takeState(pdMS_TO_TICKS(20))) return;
    const bool requested_present = present && g_driver.device.loaded;
    if (g_driver.selected_tag_present != requested_present ||
        g_driver.device.presented != requested_present) {
        g_driver.rotation_pending = false;
        g_driver.selected_tag_present = requested_present;
        pa_device_present(&g_driver.device, requested_present);
    }
    giveState();
}

void SwitchUsbTransport::setAutoTagRotation(bool enabled) {
    if (!takeState(portMAX_DELAY)) return;
    g_driver.auto_tag_rotation = enabled;
    if (!enabled && g_driver.rotation_pending) {
        g_driver.rotation_pending = false;
        pa_device_present(&g_driver.device, g_driver.selected_tag_present);
    }
    giveState();
}

bool SwitchUsbTransport::replaceTagWhenIdle(const AmiiboTag& expected,
                                           const AmiiboTag& replacement, bool present,
                                           uint32_t completed_reads, bool require_pending) {
    if (!replacement.isValid() || !replacement.verifyBcc() ||
        replacement.data()[0] != 0x04 || !takeState(pdMS_TO_TICKS(20))) {
        return false;
    }
    const bool current = g_driver.device.loaded && !g_driver.device.nfc.operation_active &&
        g_driver.selected_tag_present == present &&
        g_driver.device.completed_reads == completed_reads &&
        std::memcmp(g_driver.device.raw, expected.data(), AmiiboTag::size()) == 0;
    const bool ready = g_driver.rotation_pending ? rotationReady(millis()) : !require_pending;
    if (current && ready) {
        pa_device_load_raw(&g_driver.device, replacement.data(), AmiiboTag::size());
        pa_device_present(&g_driver.device, present);
        g_driver.rotation_pending = false;
    }
    giveState();
    return current && ready;
}

void SwitchUsbTransport::randomizeActiveUid() {
    const uint32_t random0 = esp_random();
    const uint32_t random1 = esp_random();
    if (!takeState(pdMS_TO_TICKS(20))) return;

    AmiiboTag randomized;
    if (g_driver.device.loaded && randomized.loadBin(g_driver.device.raw,
                                                      sizeof(g_driver.device.raw))) {
        randomized.randomizeUid(random0, random1);
        const bool loaded = pa_device_load_raw(&g_driver.device, randomized.data(),
                                               AmiiboTag::size());
        g_driver.selected_tag_present = loaded && g_driver.selected_tag_present;
        pa_device_present(&g_driver.device, g_driver.selected_tag_present);
    }
    giveState();
}

bool SwitchUsbTransport::consumeModifiedTag(AmiiboTag&) {
    return false;
}

ProtocolSnapshot SwitchUsbTransport::snapshot() const {
    ProtocolSnapshot snapshot{};
    if (!takeState(pdMS_TO_TICKS(5))) return snapshot;

    if (g_driver.mounted) {
        snapshot.stage = ProtocolStage::Attached;
        snapshot.hid_ready = pa_identity_streaming();
        if (snapshot.hid_ready) {
            snapshot.input_mode = pa_identity_report_id();
            snapshot.stage = ProtocolStage::Ready30;
        } else if (g_driver.rx_packets != 0) {
            snapshot.stage = ProtocolStage::Handshake80;
        }
    }
    if (g_driver.device.nfc.scan_active ||
        g_driver.device.nfc.operation_active)
        snapshot.stage = ProtocolStage::Ready31Nfc;
    snapshot.last_out_report = g_driver.last_out_report;
    snapshot.last_subcmd = g_driver.last_subcmd;
    snapshot.rx_packets = g_driver.rx_packets;
    snapshot.tx_packets = g_driver.tx_packets;

    snapshot.nfc.poll_count = g_driver.nfc_poll_count;
    snapshot.nfc.read_count = g_driver.device.read_count;
    snapshot.nfc.write_count = 0;
    snapshot.nfc.completed_reads = g_driver.device.completed_reads;
    snapshot.nfc.read_in_progress = g_driver.device.nfc.operation_active;
    snapshot.nfc.rotation_pending = rotationReady(millis());
    snapshot.nfc.power_state = !g_driver.mounted
                                   ? McuPowerState::Suspended
                                   : McuPowerState::Ready;
    snapshot.nfc.tag_present = g_driver.device.presented &&
                               !g_driver.device.nfc.tag_ejected;
    snapshot.nfc.write_dirty = false;
    if (g_driver.device.nfc.scan_active ||
        g_driver.device.nfc.operation_active)
        snapshot.nfc.power_state = McuPowerState::ConfiguredNfc;
    if (g_driver.device.nfc.operation_active)
        snapshot.nfc.nfc_state = NfcState::PendingRead;
    else if (g_driver.device.nfc.scan_active)
        snapshot.nfc.nfc_state = NfcState::Poll;

    giveState();
    return snapshot;
}

bool SwitchUsbTransport::isMounted() const {
    if (!takeState(pdMS_TO_TICKS(5))) return false;
    const bool mounted = g_driver.mounted;
    giveState();
    return mounted;
}

bool SwitchUsbTransport::setImportEnabled(bool enabled) {
    if (!takeState(pdMS_TO_TICKS(10))) return false;
    g_driver.import_enabled = enabled;
    g_driver.import_pending = false;
    g_driver.import_reply_pending = false;
    g_driver.import_report.fill(0);
    g_driver.import_reply.fill(0);
    giveState();
    return true;
}

bool SwitchUsbTransport::takeImportReport(UsbImportReport& report) {
    if (!takeState(pdMS_TO_TICKS(5))) return false;
    const bool pending = g_driver.mounted && g_driver.import_enabled &&
                         g_driver.import_pending;
    if (pending) {
        report = g_driver.import_report;
        g_driver.import_pending = false;
        g_driver.import_report.fill(0);
    }
    giveState();
    return pending;
}

void SwitchUsbTransport::replyToImportReport(const UsbImportReport& report) {
    if (report[0] != 0x81 ||
        std::memcmp(report.data() + 1, kImportMarker, sizeof(kImportMarker)) != 0 ||
        !takeState(pdMS_TO_TICKS(5)))
        return;

    if (g_driver.mounted && g_driver.import_enabled &&
        !g_driver.import_reply_pending && !g_driver.import_reply_in_flight) {
        g_driver.import_reply = report;
        g_driver.import_reply_pending = true;
    }
    giveState();
}

}  // namespace amiibo

extern "C" uint8_t const* tud_descriptor_device_cb(void) {
    return ns2_device_descriptor();
}

extern "C" uint8_t const* tud_descriptor_configuration_cb(uint8_t) {
    return ns2_config_descriptor();
}

extern "C" uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t) {
    static uint16_t descriptor[64];
    if (index == 0) {
        descriptor[0] = 0x0304;
        descriptor[1] = 0x0409;
        return descriptor;
    }

    size_t count = 0;
    const char** strings = ns2_string_table(&count);
    if (!strings || index >= count) return nullptr;

    const char* source = strings[index];
    const size_t length = std::min<size_t>(std::strlen(source), 63);
    descriptor[0] = static_cast<uint16_t>(0x0300u | (2u * length + 2u));
    for (size_t i = 0; i < length; ++i)
        descriptor[i + 1] = static_cast<uint8_t>(source[i]);
    return descriptor;
}

extern "C" bool tinyusb_vendor_control_request_cb(
    uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    if (!request) return false;
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) return false;

    if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
        size_t size = 0;
        const uint8_t* data = pa_identity_ep0(request->bRequest, &size);
        if (!data || size == 0) return false;
        const uint16_t length = std::min<uint16_t>(
            request->wLength, static_cast<uint16_t>(size));
        return tud_control_xfer(rhport, request, const_cast<uint8_t*>(data), length);
    }

    if (request->bRequest == 4 && request->wLength == 0)
        return tud_control_status(rhport, request);
    return false;
}

extern "C" void tud_mount_cb(void) {
    using namespace amiibo;
    if (takeState(pdMS_TO_TICKS(20))) {
        g_driver.mounted = true;
        g_driver.vendor_in_stage = VendorInStage::Idle;
        g_driver.vendor_zlp_required = false;
        g_driver.import_pending = false;
        g_driver.import_reply_pending = false;
        g_driver.import_reply_in_flight = false;
        g_driver.import_report.fill(0);
        g_driver.import_reply.fill(0);
        g_driver.last_hid_report_ms = millis() - 1u;
        amiibo::resetNfcSession();
        pa_identity_init();
        giveState();
    }
    ns2_vendor_rx_init(&g_driver.vendor_rx);
    ns2_vendor_tx_init(&g_driver.vendor_tx);
    resetResponseQueue();
}

extern "C" void tud_umount_cb(void) {
    amiibo::resetSessionState();
}

extern "C" usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count) {
    if (driver_count) *driver_count = 1;
    return &amiibo::kSwitch2AppDriver;
}
