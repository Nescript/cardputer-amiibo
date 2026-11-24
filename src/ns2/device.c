#include "pa_device.h"
#include "pa_identity.h"
#include <string.h>

static uint16_t clamp_axis(uint16_t value) {
    return value > 0x0FFFu ? 0x0FFFu : value;
}

static void pack_stick_pair(uint8_t *out, uint16_t x, uint16_t y) {
    x = clamp_axis(x);
    y = clamp_axis(y);
    out[0] = (uint8_t)x;
    out[1] = (uint8_t)(((x >> 8) & 0x0Fu) | ((y & 0x0Fu) << 4));
    out[2] = (uint8_t)(y >> 4);
}

static void clear_read_tracking(pa_device_t *d) {
    d->read_tracking_active = false;
    memset(d->read_coverage, 0, sizeof(d->read_coverage));
}

static bool read_coverage_complete(const pa_device_t *d) {
    for (size_t i = 0; i < sizeof(d->read_coverage); ++i) {
        if (d->read_coverage[i] != 0xFFu) return false;
    }
    return true;
}

static void record_read_chunk(pa_device_t *d, const uint8_t *command,
                              size_t command_size,
                              const ns2_virtual_nfc_response_t *response) {
    if (!d->read_tracking_active) return;

    if (command_size < 10u || !d->nfc.operation_active ||
        d->nfc.write_mode ||
        d->nfc.operation_buffer_size != NS2_NFC_READ_BUFFER_SIZE ||
        response->response_direction != 0x01 ||
        response->payload_size < NS2_NFC_READ_CHUNK_HEADER_SIZE) {
        clear_read_tracking(d);
        return;
    }

    const uint16_t offset =
        (uint16_t)command[8] | ((uint16_t)command[9] << 8);
    const size_t length = (size_t)response->payload[1] |
                          ((size_t)response->payload[2] << 8);
    if (length == 0 || length > NS2_NFC_READ_CHUNK_DATA_SIZE ||
        response->payload_size !=
            NS2_NFC_READ_CHUNK_HEADER_SIZE + length ||
        (size_t)offset + length > NS2_NFC_READ_BUFFER_SIZE ||
        response->payload[0] !=
            ((size_t)offset + length == NS2_NFC_READ_BUFFER_SIZE ? 1u : 0u)) {
        clear_read_tracking(d);
        return;
    }

    for (size_t i = offset; i < (size_t)offset + length; ++i)
        d->read_coverage[i / 8u] |= (uint8_t)(1u << (i % 8u));
}

void pa_device_init(pa_device_t *d) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    pa_device_set_sticks(d, 2048, 2048, 2048, 2048);
    ns2_virtual_nfc_runtime_init(&d->nfc);
    pa_identity_init();
}

void pa_device_reset_nfc(pa_device_t *d) {
    if (!d) return;
    ns2_virtual_nfc_runtime_init(&d->nfc);
    clear_read_tracking(d);
}

bool pa_device_read_in_progress(const pa_device_t *d) {
    return d && d->read_tracking_active && d->nfc.operation_active &&
           !d->nfc.write_mode;
}

void pa_device_present(pa_device_t *d, bool present) {
    if (!d) return;
    const bool requested = present && d->loaded;
    if (d->presented == requested) return;
    d->presented = requested;
    d->nfc.media_changed = true;
}

bool pa_device_load_raw(pa_device_t *d, const uint8_t *data, size_t size) {
    if (!d) return false;
    d->loaded = false;
    d->presented = false;
    d->nfc.media_changed = true;
    if (!data || !virtual_amiibo_supported_size(size) ||
        virtual_amiibo_validate_raw(data, VIRTUAL_AMIIBO_RAW_SIZE) !=
            VIRTUAL_AMIIBO_OK) {
        memset(d->raw, 0, sizeof(d->raw));
        memset(d->signature, 0, sizeof(d->signature));
        return false;
    }

    memmove(d->raw, data, VIRTUAL_AMIIBO_RAW_SIZE);
    if (size == VIRTUAL_AMIIBO_EXTENDED_SIZE)
        memmove(d->signature, data + VIRTUAL_AMIIBO_RAW_SIZE,
                VIRTUAL_AMIIBO_SIGNATURE_SIZE);
    else
        memset(d->signature, 0, sizeof(d->signature));
    d->loaded = true;
    return true;
}

void pa_device_set_sticks(pa_device_t *d, uint16_t lx, uint16_t ly,
                          uint16_t rx, uint16_t ry) {
    if (!d) return;
    d->sticks[0] = clamp_axis(lx);
    d->sticks[1] = clamp_axis(ly);
    d->sticks[2] = clamp_axis(rx);
    d->sticks[3] = clamp_axis(ry);
}

size_t pa_device_command(pa_device_t *d, uint32_t now_ms,
                         const uint8_t *c, size_t n, uint8_t r[128]) {
    if (!d || !r) return 0;
    if (!c || n < 8 || n != 8u + ((size_t)c[4]<<8) + c[5] || c[1] != 0x91 || c[2] != 0) {
        ++d->rejected;
        return 0;
    }
    ++d->commands;
    if (c[0] != 1) return pa_identity_command(c, n, r);
    ++d->nfc_commands;
    const bool media_changed = d->nfc.media_changed;
    if (media_changed) clear_read_tracking(d);
    const bool complete_read_stop =
        c[3] == 0x04 && !media_changed && d->read_tracking_active &&
        d->nfc.operation_active && !d->nfc.write_mode &&
        d->nfc.operation_buffer_size == NS2_NFC_READ_BUFFER_SIZE &&
        read_coverage_complete(d);
    ns2_virtual_nfc_response_t response;
    // P0 is read-only. Do not acknowledge a write that is not durable.
    // Preserve the upstream error event/status instead of silently losing writes.
    if (c[3] == 0x14 || c[3] == 0x08) {
        ns2_virtual_nfc_runtime_write_apply_failed(&d->nfc, now_ms);
        memset(&response, 0, sizeof(response));
        response.response_direction = 4;
        ++d->rejected;
    } else if (!ns2_virtual_nfc_runtime_dispatch(&d->nfc, now_ms, c[3], c+8, n-8,
                 d->presented, d->presented ? d->raw : NULL, d->signature, &response)) {
        clear_read_tracking(d);
        return pa_identity_command(c, n, r);
    }
    if (c[3] == 0x15 && response.response_direction == 0x01 &&
        response.payload_size > NS2_NFC_READ_CHUNK_HEADER_SIZE)
        ++d->read_count;
    if (c[3] == 0x06) {
        clear_read_tracking(d);
        if (response.response_direction == 0x04 &&
            response.payload_size == 0 && d->nfc.operation_active &&
            !d->nfc.write_mode &&
            d->nfc.operation_buffer_size == NS2_NFC_READ_BUFFER_SIZE)
            d->read_tracking_active = true;
    } else if (c[3] == 0x15) {
        record_read_chunk(d, c, n, &response);
    } else if (c[3] == 0x04) {
        clear_read_tracking(d);
    } else if (d->read_tracking_active &&
               (!d->nfc.operation_active || d->nfc.write_mode ||
                d->nfc.operation_buffer_size != NS2_NFC_READ_BUFFER_SIZE)) {
        clear_read_tracking(d);
    }
    memset(r, 0, 128);
    r[0] = 1; r[1] = response.response_direction; r[2] = c[2]; r[3] = c[3]; r[5] = 0xf8;
    memcpy(r+8, response.payload, response.payload_size);
    const size_t response_size = 8u + response.payload_size;
    if (complete_read_stop && response_size == 8u && r[0] == 1 &&
        r[1] == 0x04 && r[3] == 0x04 && d->completed_reads != UINT32_MAX)
        ++d->completed_reads;
    return response_size;
}

void pa_device_report(pa_device_t *d, uint32_t now_ms, const uint8_t buttons[3], uint8_t r[63]) {
    if (!d || !r) return;
    ns2_virtual_nfc_runtime_report_tick(&d->nfc, now_ms, d->presented);
    memset(r, 0, 63);
    r[0] = d->counter++;
    r[1] = 0x25; // Upstream unknown-source fallback: external power, full.
    if (buttons) memcpy(r+2, buttons, 3);
    pack_stick_pair(r + 5, d->sticks[0], d->sticks[1]);
    pack_stick_pair(r + 8, d->sticks[2], d->sticks[3]);
    r[11] = 0x30;
    r[12] = ns2_virtual_nfc_runtime_report_state(&d->nfc);
    // No motion or headset is advertised.
}
