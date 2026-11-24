#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ns2_virtual_nfc_runtime.h"

#define PA_DEVICE_NFC_READ_COVERAGE_SIZE \
    ((NS2_NFC_READ_BUFFER_SIZE + 7u) / 8u)

typedef struct {
    ns2_virtual_nfc_runtime_t nfc;
    uint8_t raw[540];
    uint8_t signature[32];
    uint8_t read_coverage[PA_DEVICE_NFC_READ_COVERAGE_SIZE];
    uint8_t counter;
    uint16_t sticks[4];
    bool loaded;
    bool presented;
    bool read_tracking_active;
    uint32_t commands;
    uint32_t nfc_commands;
    uint32_t rejected;
    uint32_t read_count;
    uint32_t completed_reads;
} pa_device_t;

#ifdef __cplusplus
extern "C" {
#endif

void pa_device_init(pa_device_t *device);
// Reset per-session NFC runtime and transaction state. Diagnostic and
// completion counters are preserved for the lifetime of the firmware.
void pa_device_reset_nfc(pa_device_t *device);
// True while an accepted read operation is awaiting Stop.
bool pa_device_read_in_progress(const pa_device_t *device);
void pa_device_present(pa_device_t *device, bool present);
bool pa_device_load_raw(pa_device_t *device, const uint8_t *data, size_t size);
void pa_device_set_sticks(pa_device_t *device, uint16_t lx, uint16_t ly,
                          uint16_t rx, uint16_t ry);
size_t pa_device_command(pa_device_t *device, uint32_t now_ms,
                         const uint8_t *command, size_t size, uint8_t response[128]);
void pa_device_report(pa_device_t *device, uint32_t now_ms,
                      const uint8_t buttons[3], uint8_t response[63]);

#ifdef __cplusplus
}
#endif
