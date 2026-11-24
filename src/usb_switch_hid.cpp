#include "usb_switch_hid.h"

#include <Arduino.h>
#include <USB.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstring>

extern "C" {
#include "device/usbd_pvt.h"
#include "esp32-hal-tinyusb.h"
}

namespace amiibo {

namespace {

constexpr uint8_t kEpInAddr = 0x81;
constexpr uint8_t kEpOutAddr = 0x01;
constexpr uint16_t kEpMaxPacketSize = 64;
constexpr size_t kTxReplyQueueCap = 12;
constexpr uint8_t kImportRequestPrefix[] = {0x80, 0xA5, 'A', 'M', 'B', '1'};

// Exact 203-byte Nintendo Switch Pro Controller HID Report Descriptor
// (0x30 Gamepad + 0x21/0x81 Input Vendor + 0x01/0x10/0x80/0x82 Output Vendor)
constexpr uint8_t kSwitchProReportDescriptor[] = {
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x15, 0x00,                    // Logical Minimum (0)
    0x09, 0x04,                    // Usage (Joystick)
    0xA1, 0x01,                    // Collection (Application)
    0x85, 0x30,                    //   Report ID (0x30)
    0x05, 0x01,                    //   Usage Page (Generic Desktop)
    0x05, 0x09,                    //   Usage Page (Button)
    0x19, 0x01,                    //   Usage Minimum (Button 1)
    0x29, 0x0A,                    //   Usage Maximum (Button 10)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x0A,                    //   Report Count (10)
    0x55, 0x00,                    //   Unit Exponent (0)
    0x65, 0x00,                    //   Unit (None)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x05, 0x09,                    //   Usage Page (Button)
    0x19, 0x0B,                    //   Usage Minimum (Button 11)
    0x29, 0x0E,                    //   Usage Maximum (Button 14)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x04,                    //   Report Count (4)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x02,                    //   Report Count (2)
    0x81, 0x03,                    //   Input (Const,Var,Abs)
    0x0B, 0x01, 0x00, 0x01, 0x00,  //   Usage (Generic Desktop: Pointer)
    0xA1, 0x00,                    //   Collection (Physical)
    0x0B, 0x30, 0x00, 0x01, 0x00,  //     Usage (X)
    0x0B, 0x31, 0x00, 0x01, 0x00,  //     Usage (Y)
    0x0B, 0x32, 0x00, 0x01, 0x00,  //     Usage (Z)
    0x0B, 0x35, 0x00, 0x01, 0x00,  //     Usage (Rz)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65535)
    0x75, 0x10,                    //     Report Size (16)
    0x95, 0x04,                    //     Report Count (4)
    0x81, 0x02,                    //     Input (Data,Var,Abs)
    0xC0,                          //   End Collection
    0x0B, 0x39, 0x00, 0x01, 0x00,  //   Usage (Hat Switch)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x07,                    //   Logical Maximum (7)
    0x35, 0x00,                    //   Physical Minimum (0)
    0x46, 0x3B, 0x01,              //   Physical Maximum (315)
    0x65, 0x14,                    //   Unit (Degrees)
    0x75, 0x04,                    //   Report Size (4)
    0x95, 0x01,                    //   Report Count (1)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x05, 0x09,                    //   Usage Page (Button)
    0x19, 0x0F,                    //   Usage Minimum (Button 15)
    0x29, 0x12,                    //   Usage Maximum (Button 18)
    0x15, 0x00,                    //   Logical Minimum (0)
    0x25, 0x01,                    //   Logical Maximum (1)
    0x75, 0x01,                    //   Report Size (1)
    0x95, 0x04,                    //   Report Count (4)
    0x81, 0x02,                    //   Input (Data,Var,Abs)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x34,                    //   Report Count (52)
    0x81, 0x03,                    //   Input (Const,Var,Abs)
    0x06, 0x00, 0xFF,              //   Usage Page (Vendor Defined 0xFF00)
    0x85, 0x21,                    //   Report ID (0x21)
    0x09, 0x01,                    //   Usage (0x01)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x81, 0x03,                    //   Input (Const,Var,Abs)
    0x85, 0x81,                    //   Report ID (0x81)
    0x09, 0x02,                    //   Usage (0x02)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x81, 0x03,                    //   Input (Const,Var,Abs)
    0x85, 0x01,                    //   Report ID (0x01)
    0x09, 0x03,                    //   Usage (0x03)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x91, 0x83,                    //   Output (Const,Var,Abs,Volatile)
    0x85, 0x10,                    //   Report ID (0x10)
    0x09, 0x04,                    //   Usage (0x04)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x91, 0x83,                    //   Output (Const,Var,Abs,Volatile)
    0x85, 0x80,                    //   Report ID (0x80)
    0x09, 0x05,                    //   Usage (0x05)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x91, 0x83,                    //   Output (Const,Var,Abs,Volatile)
    0x85, 0x82,                    //   Report ID (0x82)
    0x09, 0x06,                    //   Usage (0x06)
    0x75, 0x08,                    //   Report Size (8)
    0x95, 0x3F,                    //   Report Count (63)
    0x91, 0x83,                    //   Output (Const,Var,Abs,Volatile)
    0xC0                           // End Collection
};

constexpr uint8_t kConfigDescriptor[41] = {
    // Configuration Descriptor (9 bytes)
    0x09,
    TUSB_DESC_CONFIGURATION,
    41, 0x00,                      // wTotalLength = 41
    0x01,                          // bNumInterfaces = 1
    0x01,                          // bConfigurationValue = 1
    0x00,                          // iConfiguration = 0
    TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP | 0x80,  // Bus-powered + Remote Wakeup (0xA0)
    250,                           // bMaxPower = 500 mA

    // Interface 0 Descriptor (9 bytes)
    0x09,
    TUSB_DESC_INTERFACE,
    0x00,                          // bInterfaceNumber = 0
    0x00,                          // bAlternateSetting = 0
    0x02,                          // bNumEndpoints = 2
    TUSB_CLASS_HID,                // bInterfaceClass = 0x03 (HID)
    0x00,                          // bInterfaceSubClass = 0
    0x00,                          // bInterfaceProtocol = 0
    0x00,                          // iInterface = 0

    // HID Descriptor (9 bytes)
    0x09,
    0x21,                          // HID Descriptor Type
    0x11, 0x01,                    // bcdHID = 1.11
    0x00,                          // bCountryCode = 0
    0x01,                          // bNumDescriptors = 1
    0x22,                          // bDescriptorType = Report (0x22)
    static_cast<uint8_t>(sizeof(kSwitchProReportDescriptor) & 0xFFU),
    static_cast<uint8_t>((sizeof(kSwitchProReportDescriptor) >> 8U) & 0xFFU),

    // Endpoint IN Descriptor (7 bytes)
    0x07,
    TUSB_DESC_ENDPOINT,
    kEpInAddr,
    TUSB_XFER_INTERRUPT,
    kEpMaxPacketSize, 0x00,
    8,                             // bInterval = 8 ms (125 Hz)

    // Endpoint OUT Descriptor (7 bytes)
    0x07,
    TUSB_DESC_ENDPOINT,
    kEpOutAddr,
    TUSB_XFER_INTERRUPT,
    kEpMaxPacketSize, 0x00,
    8,                             // bInterval = 8 ms
};

// TinyUSB endpoint DMA-aligned buffers (512 bytes supports full 362-byte 0x31 reports)
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t g_ep_in_buf[512];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t g_ep_out_buf[64];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN uint8_t g_ctrl_buf[64];

struct DriverState {
    uint8_t rhport{0};
    uint8_t itf_num{0};
    uint8_t ep_in{0};
    uint8_t ep_out{0};
    uint8_t hid_protocol{1};
    uint8_t hid_idle{0};
    volatile bool mounted{false};

    SemaphoreHandle_t proto_mutex{nullptr};
    TaskHandle_t tx_task_handle{nullptr};

    SwitchProtocol protocol{};
    OutgoingReport tx_queue[kTxReplyQueueCap]{};
    size_t q_head{0};
    size_t q_tail{0};
    size_t q_count{0};

    bool import_enabled{false};
    bool import_pending{false};
    UsbImportReport import_report{};
};

DriverState g_drv;

// Caller must hold g_drv.proto_mutex
void enqueueTxLocked(const OutgoingReport& rep) {
    if (g_drv.q_count >= kTxReplyQueueCap) {
        return;
    }
    g_drv.tx_queue[g_drv.q_tail] = rep;
    g_drv.q_tail = (g_drv.q_tail + 1) % kTxReplyQueueCap;
    ++g_drv.q_count;
}

bool dequeueTxLocked(OutgoingReport& out) {
    if (g_drv.q_count == 0) {
        return false;
    }
    out = g_drv.tx_queue[g_drv.q_head];
    g_drv.q_head = (g_drv.q_head + 1) % kTxReplyQueueCap;
    --g_drv.q_count;
    return true;
}

void clearTxQueueLocked() {
    g_drv.q_head = 0;
    g_drv.q_tail = 0;
    g_drv.q_count = 0;
}

void processHostPacket(const uint8_t* data, size_t len) {
    if (!g_drv.proto_mutex || !data || len == 0) {
        return;
    }
    if (xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (len == g_drv.import_report.size() &&
            std::memcmp(data, kImportRequestPrefix, sizeof(kImportRequestPrefix)) == 0) {
            if (g_drv.import_enabled && !g_drv.import_pending) {
                std::memcpy(g_drv.import_report.data(), data, len);
                g_drv.import_pending = true;
            }
        } else {
            OutgoingReport reply{};
            if (g_drv.protocol.onHostReport(data, len, reply)) {
                enqueueTxLocked(reply);
                if (g_drv.tx_task_handle) {
                    xTaskNotifyGive(g_drv.tx_task_handle);
                }
            }
        }
        xSemaphoreGive(g_drv.proto_mutex);
    }
}

void drv_init(void) {}

void drv_reset(uint8_t rhport) {
    (void)rhport;
    g_drv.mounted = false;
    g_drv.ep_in = 0;
    g_drv.ep_out = 0;
    if (g_drv.proto_mutex && xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        clearTxQueueLocked();
        g_drv.import_pending = false;
        g_drv.protocol.onDetach();
        xSemaphoreGive(g_drv.proto_mutex);
    }
}

uint16_t drv_open(uint8_t rhport, tusb_desc_interface_t const* desc_intf, uint16_t max_len) {
    if (!desc_intf || desc_intf->bInterfaceClass != TUSB_CLASS_HID) {
        return 0;
    }
    if (max_len < sizeof(tusb_desc_interface_t) + 9 + 2 * sizeof(tusb_desc_endpoint_t)) {
        return 0;
    }

    uint8_t const* p_desc = reinterpret_cast<uint8_t const*>(desc_intf);
    uint16_t drv_len = sizeof(tusb_desc_interface_t);
    p_desc = tu_desc_next(p_desc);

    // Skip HID descriptor (9 bytes)
    if (tu_desc_type(p_desc) == 0x21) {
        drv_len += tu_desc_len(p_desc);
        p_desc = tu_desc_next(p_desc);
    }

    uint8_t found_in = 0;
    uint8_t found_out = 0;
    for (uint8_t i = 0; i < desc_intf->bNumEndpoints; ++i) {
        if (drv_len + sizeof(tusb_desc_endpoint_t) > max_len) {
            return 0;
        }
        if (tu_desc_type(p_desc) != TUSB_DESC_ENDPOINT) {
            return 0;
        }
        auto const* desc_ep = reinterpret_cast<tusb_desc_endpoint_t const*>(p_desc);
        if (!usbd_edpt_open(rhport, desc_ep)) {
            return 0;
        }
        if (tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_IN) {
            found_in = desc_ep->bEndpointAddress;
        } else {
            found_out = desc_ep->bEndpointAddress;
        }
        drv_len += tu_desc_len(p_desc);
        p_desc = tu_desc_next(p_desc);
    }

    if (found_in == 0 || found_out == 0) {
        return 0;
    }

    g_drv.rhport = rhport;
    g_drv.itf_num = desc_intf->bInterfaceNumber;
    g_drv.ep_in = found_in;
    g_drv.ep_out = found_out;
    g_drv.mounted = true;

    // Arm OUT endpoint to receive Switch commands
    usbd_edpt_xfer(rhport, g_drv.ep_out, g_ep_out_buf, sizeof(g_ep_out_buf));

    // Enqueue proactive 0x81 0x01 attach report
    if (g_drv.proto_mutex && xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        clearTxQueueLocked();
        OutgoingReport init_rep{};
        g_drv.protocol.onAttach(init_rep);
        enqueueTxLocked(init_rep);
        xSemaphoreGive(g_drv.proto_mutex);
    }
    if (g_drv.tx_task_handle) {
        xTaskNotifyGive(g_drv.tx_task_handle);
    }

    return drv_len;
}

bool drv_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    if (!request) {
        return false;
    }
    if (request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE ||
        request->wIndex != g_drv.itf_num) {
        return false;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        if (request->bRequest == TUSB_REQ_GET_DESCRIPTOR) {
            const uint8_t desc_type = static_cast<uint8_t>(request->wValue >> 8U);
            if (desc_type == 0x22) {
                return tud_control_xfer(rhport, request,
                                        const_cast<uint8_t*>(kSwitchProReportDescriptor),
                                        sizeof(kSwitchProReportDescriptor));
            }
            if (desc_type == 0x21) {
                return tud_control_xfer(rhport, request,
                                        const_cast<uint8_t*>(kConfigDescriptor + 18), 9);
            }
        }
        return false;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS) {
        switch (request->bRequest) {
        case 0x01:  // GET_REPORT
            if (stage == CONTROL_STAGE_SETUP) {
                std::memset(g_ctrl_buf, 0, sizeof(g_ctrl_buf));
                g_ctrl_buf[0] = 0x30;
                const uint16_t len = request->wLength < sizeof(g_ctrl_buf) ? request->wLength
                                                                           : sizeof(g_ctrl_buf);
                return tud_control_xfer(rhport, request, g_ctrl_buf, len);
            }
            return true;
        case 0x02:  // GET_IDLE
            if (stage == CONTROL_STAGE_SETUP) {
                g_ctrl_buf[0] = g_drv.hid_idle;
                return tud_control_xfer(rhport, request, g_ctrl_buf, 1);
            }
            return true;
        case 0x03:  // GET_PROTOCOL
            if (stage == CONTROL_STAGE_SETUP) {
                g_ctrl_buf[0] = g_drv.hid_protocol;
                return tud_control_xfer(rhport, request, g_ctrl_buf, 1);
            }
            return true;
        case 0x09:  // SET_REPORT
            if (stage == CONTROL_STAGE_SETUP) {
                const uint16_t len = request->wLength < sizeof(g_ctrl_buf) ? request->wLength
                                                                           : sizeof(g_ctrl_buf);
                return tud_control_xfer(rhport, request, g_ctrl_buf, len);
            }
            if (stage == CONTROL_STAGE_ACK) {
                const uint16_t len = request->wLength < sizeof(g_ctrl_buf) ? request->wLength
                                                                           : sizeof(g_ctrl_buf);
                processHostPacket(g_ctrl_buf, len);
            }
            return true;
        case 0x0A:  // SET_IDLE
            if (stage == CONTROL_STAGE_SETUP) {
                g_drv.hid_idle = static_cast<uint8_t>(request->wValue >> 8U);
                return tud_control_status(rhport, request);
            }
            return true;
        case 0x0B:  // SET_PROTOCOL
            if (stage == CONTROL_STAGE_SETUP) {
                g_drv.hid_protocol = static_cast<uint8_t>(request->wValue & 0xFFU);
                return tud_control_status(rhport, request);
            }
            return true;
        default:
            return false;
        }
    }

    return false;
}

bool drv_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    if (ep_addr == g_drv.ep_out) {
        if (result == XFER_RESULT_SUCCESS && xferred_bytes > 0) {
            uint8_t rx_copy[64]{};
            const size_t n = xferred_bytes < sizeof(rx_copy) ? xferred_bytes : sizeof(rx_copy);
            std::memcpy(rx_copy, g_ep_out_buf, n);
            // Re-arm OUT endpoint immediately before processing
            usbd_edpt_xfer(rhport, g_drv.ep_out, g_ep_out_buf, sizeof(g_ep_out_buf));
            processHostPacket(rx_copy, n);
        } else {
            usbd_edpt_xfer(rhport, g_drv.ep_out, g_ep_out_buf, sizeof(g_ep_out_buf));
        }
        return true;
    }

    if (ep_addr == g_drv.ep_in) {
        if (g_drv.tx_task_handle) {
            xTaskNotifyGive(g_drv.tx_task_handle);
        }
        return true;
    }

    return false;
}

const usbd_class_driver_t kSwitchHidDriver = {
#if CFG_TUSB_DEBUG >= 2
    .name = "SwitchProHID",
#endif
    .init = drv_init,
    .reset = drv_reset,
    .open = drv_open,
    .control_xfer_cb = drv_control_xfer_cb,
    .xfer_cb = drv_xfer_cb,
    .sof = nullptr,
};

bool sendReportOverUsb(const OutgoingReport& rep) {
    if (!g_drv.mounted || g_drv.ep_in == 0 || rep.length == 0 || !tud_ready()) {
        return false;
    }
    if (!usbd_edpt_claim(g_drv.rhport, g_drv.ep_in)) {
        return false;
    }
    std::memcpy(g_ep_in_buf, rep.data.data(), rep.length);
    if (!usbd_edpt_xfer(g_drv.rhport, g_drv.ep_in, g_ep_in_buf, rep.length)) {
        usbd_edpt_release(g_drv.rhport, g_drv.ep_in);
        return false;
    }
    return true;
}

}  // namespace

SwitchUsbTransport& SwitchUsbTransport::instance() {
    static SwitchUsbTransport inst;
    return inst;
}

bool SwitchUsbTransport::begin() {
    if (!g_drv.proto_mutex) {
        g_drv.proto_mutex = xSemaphoreCreateMutex();
        if (!g_drv.proto_mutex) {
            return false;
        }
    }

    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        g_drv.protocol.setMacAddress(mac);
    }

    if (!g_drv.tx_task_handle) {
        xTaskCreatePinnedToCore(usbTxTaskEntry, "usb_tx_task", 4096, this, 5,
                                &g_drv.tx_task_handle, 0);
    }

    USB.VID(0x057E);
    USB.PID(0x2009);
    USB.firmwareVersion(0x0210);
    USB.usbVersion(0x0200);
    USB.usbClass(0x00);
    USB.usbSubClass(0x00);
    USB.usbProtocol(0x00);
    USB.usbAttributes(TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP);
    USB.usbPower(500);
    USB.webUSB(false);
    USB.manufacturerName("Nintendo Co., Ltd.");
    USB.productName("Pro Controller");
    USB.serialNumber("000000000001");
    return USB.begin();
}

void SwitchUsbTransport::setControllerState(const ControllerState& state) {
    if (g_drv.proto_mutex && xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        g_drv.protocol.setControllerState(state);
        xSemaphoreGive(g_drv.proto_mutex);
    }
}

void SwitchUsbTransport::loadAndArmTag(const AmiiboTag& tag, bool present) {
    if (g_drv.proto_mutex && xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_drv.protocol.mcu().setTag(tag, present);
        xSemaphoreGive(g_drv.proto_mutex);
    }
}

void SwitchUsbTransport::setTagPresent(bool present) {
    if (g_drv.proto_mutex && xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_drv.protocol.mcu().setTagPresent(present);
        xSemaphoreGive(g_drv.proto_mutex);
    }
}

void SwitchUsbTransport::randomizeActiveUid() {
    const uint32_t r0 = esp_random();
    const uint32_t r1 = esp_random();
    if (g_drv.proto_mutex && xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_drv.protocol.mcu().randomizeArmedUid(r0, r1);
        xSemaphoreGive(g_drv.proto_mutex);
    }
}

bool SwitchUsbTransport::consumeModifiedTag(AmiiboTag& out_tag) {
    bool modified = false;
    if (g_drv.proto_mutex && xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (g_drv.protocol.mcu().consumeWriteDirty()) {
            out_tag = g_drv.protocol.mcu().activeTag();
            modified = true;
        }
        xSemaphoreGive(g_drv.proto_mutex);
    }
    return modified;
}

ProtocolSnapshot SwitchUsbTransport::snapshot() const {
    ProtocolSnapshot snap{};
    if (g_drv.proto_mutex && xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        snap = g_drv.protocol.snapshot();
        xSemaphoreGive(g_drv.proto_mutex);
    }
    return snap;
}

bool SwitchUsbTransport::isMounted() const {
    return g_drv.mounted;
}

bool SwitchUsbTransport::setImportEnabled(bool enabled) {
    if (!g_drv.proto_mutex || xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }
    g_drv.import_enabled = enabled;
    g_drv.import_pending = false;
    xSemaphoreGive(g_drv.proto_mutex);
    return true;
}

bool SwitchUsbTransport::takeImportReport(UsbImportReport& report) {
    if (!g_drv.proto_mutex || xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    const bool pending = g_drv.mounted && g_drv.import_pending;
    if (pending) {
        report = g_drv.import_report;
        g_drv.import_pending = false;
    }
    xSemaphoreGive(g_drv.proto_mutex);
    return pending;
}

void SwitchUsbTransport::replyToImportReport(const UsbImportReport& report) {
    if (!g_drv.proto_mutex || xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    if (g_drv.mounted && g_drv.import_enabled && g_drv.tx_task_handle) {
        OutgoingReport reply{};
        reply.length = report.size();
        std::memcpy(reply.data.data(), report.data(), report.size());
        enqueueTxLocked(reply);
        xTaskNotifyGive(g_drv.tx_task_handle);
    }
    xSemaphoreGive(g_drv.proto_mutex);
}

void SwitchUsbTransport::usbTxTaskEntry(void* arg) {
    static_cast<SwitchUsbTransport*>(arg)->usbTxTaskLoop();
}

void SwitchUsbTransport::usbTxTaskLoop() {
    TickType_t last_stream_tick = xTaskGetTickCount();
    const TickType_t stream_period = pdMS_TO_TICKS(8);

    while (true) {
        // Wait for either TX notification (new reply / IN xfer completion) or 8 ms stream period
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(8));

        if (!g_drv.mounted || !tud_ready()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 1. Drain prioritized control/handshake/subcommand replies
        while (g_drv.mounted && tud_ready()) {
            if (usbd_edpt_busy(g_drv.rhport, g_drv.ep_in)) {
                break;
            }
            OutgoingReport rep{};
            bool has_reply = false;
            if (xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                has_reply = dequeueTxLocked(rep);
                xSemaphoreGive(g_drv.proto_mutex);
            }
            if (!has_reply) {
                break;
            }
            if (!sendReportOverUsb(rep)) {
                // Re-queue at front? Rare; wait briefly for endpoint and retry once
                vTaskDelay(pdMS_TO_TICKS(1));
                sendReportOverUsb(rep);
            }
        }

        // 2. Periodically emit 0x30 (64B) or 0x31 (362B) input report at 125 Hz
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_stream_tick) >= stream_period &&
            !usbd_edpt_busy(g_drv.rhport, g_drv.ep_in)) {
            last_stream_tick = now;
            OutgoingReport stream{};
            bool ready_to_send = false;
            if (xSemaphoreTake(g_drv.proto_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                if (g_drv.q_count == 0 && g_drv.protocol.isReady()) {
                    ready_to_send = g_drv.protocol.buildStreamReport(stream);
                }
                xSemaphoreGive(g_drv.proto_mutex);
            }
            if (ready_to_send) {
                sendReportOverUsb(stream);
            }
        }
    }
}

}  // namespace amiibo

extern "C" uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return amiibo::kConfigDescriptor;
}

extern "C" usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return &amiibo::kSwitchHidDriver;
}
