#!/usr/bin/env python3
"""Compile the Switch 2 protocol core as C and exercise it from C++."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCES = [
    "device.c",
    "identity.c",
    "ns2_firmware_profile.c",
    "ns2_pairing_crypto.c",
    "ns2_vendor_rx.c",
    "ns2_vendor_tx.c",
    "ns2_virtual_nfc.c",
    "ns2_virtual_nfc_runtime.c",
    "virtual_amiibo.c",
]

HOST_TEST = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "ns2/pa_device.h"
#include "ns2/pa_identity.h"
#include "ns2/ns2_vendor_rx.h"
#include "ns2/ns2_vendor_tx.h"
#include "ns2/ns2_firmware_profile.h"
#include "ns2/ns2_pairing_crypto.h"
#include "ns2/ns2_virtual_nfc.h"
#include "ns2/ns2_virtual_nfc_runtime.h"
#include "ns2/virtual_amiibo.h"

static void make_raw(uint8_t raw[540], uint8_t seed) {
    for (size_t i = 0; i < 540; ++i)
        raw[i] = static_cast<uint8_t>(seed + i * 29u);
    raw[0] = 0x04;
    raw[1] = 0x11;
    raw[2] = 0x22;
    raw[3] = static_cast<uint8_t>(0x88u ^ raw[0] ^ raw[1] ^ raw[2]);
    raw[4] = 0x33;
    raw[5] = 0x44;
    raw[6] = 0x55;
    raw[7] = 0x66;
    raw[8] = static_cast<uint8_t>(raw[4] ^ raw[5] ^ raw[6] ^ raw[7]);
}

static size_t send(pa_device_t *device, uint32_t now_ms, uint8_t command,
                   const uint8_t *payload, size_t payload_size,
                   uint8_t response[128]) {
    assert(payload_size <= 120);
    uint8_t packet[128] = {1, 0x91, 0, command, 0,
                           static_cast<uint8_t>(payload_size), 0, 0};
    if (payload_size != 0)
        std::memcpy(packet + 8, payload, payload_size);
    return pa_device_command(device, now_ms, packet, payload_size + 8,
                             response);
}

static void start_scan(pa_device_t *device, uint32_t &now_ms) {
    uint8_t response[128]{};
    assert(send(device, now_ms++, 0x03, nullptr, 0, response) == 8);
    now_ms += 40;
    pa_device_report(device, now_ms++, nullptr, response);
}

static void test_read_buffer(pa_device_t *device, uint32_t &now_ms,
                             const uint8_t raw[540]) {
    uint8_t response[128]{};
    const uint8_t begin[19] = {0xD0, 0x07, 0, 0, 0, 0, 0, 0, 0,
                               1, 3, 0, 0x3B, 0x3C, 0x77, 0x78, 0x86, 0, 0};
    assert(send(device, now_ms++, 0x06, begin, sizeof(begin), response) == 8);

    uint8_t expected[NS2_NFC_READ_BUFFER_SIZE]{};
    assert(ns2_virtual_nfc_build_read_buffer(
               raw, device->signature, begin + 10, expected) ==
           NS2_VIRTUAL_NFC_OK);
    uint8_t received[NS2_NFC_READ_BUFFER_SIZE]{};
    size_t received_size = 0;
    for (uint16_t offset = 0; offset < NS2_NFC_READ_BUFFER_SIZE;
         offset = static_cast<uint16_t>(offset + NS2_NFC_READ_CHUNK_DATA_SIZE)) {
        const uint8_t request[2] = {static_cast<uint8_t>(offset),
                                    static_cast<uint8_t>(offset >> 8)};
        const size_t response_size =
            send(device, now_ms++, 0x15, request, sizeof(request), response);
        assert(response_size >= 11 && response[1] == 0x01);
        const size_t chunk_size = response[9] | (response[10] << 8);
        assert(response_size == 11 + chunk_size);
        assert(received_size + chunk_size <= sizeof(received));
        std::memcpy(received + received_size, response + 11, chunk_size);
        received_size += chunk_size;
        assert(response[8] == (received_size == sizeof(received)));
    }
    assert(received_size == sizeof(received));
    assert(std::memcmp(expected, received, sizeof(expected)) == 0);
}

static void begin_read_operation(pa_device_t *device, uint32_t &now_ms,
                                 uint8_t response[128]) {
    const uint8_t begin[19] = {0xD0, 0x07, 0, 0, 0, 0, 0, 0, 0,
                               1, 3, 0, 0x3B, 0x3C, 0x77, 0x78, 0x86, 0, 0};
    assert(send(device, now_ms++, 0x06, begin, sizeof(begin), response) == 8);
    assert(response[1] == 0x04);
    assert(pa_device_read_in_progress(device));
}

static size_t request_read_chunk(pa_device_t *device, uint32_t &now_ms,
                                 uint16_t offset, uint8_t response[128],
                                 bool with_ignored_tail = false) {
    const uint8_t request[4] = {static_cast<uint8_t>(offset),
                                static_cast<uint8_t>(offset >> 8), 0xA5, 0x5A};
    const size_t request_size = with_ignored_tail ? sizeof(request) : 2;
    return send(device, now_ms++, 0x15, request, request_size, response);
}

static void test_completed_read_tracking() {
    pa_device_t device{};
    pa_device_init(&device);
    uint8_t raw[540]{};
    make_raw(raw, 0x31);
    assert(pa_device_load_raw(&device, raw, sizeof(raw)));
    pa_device_present(&device, true);
    uint32_t now_ms = 1;
    uint8_t response[128]{};
    start_scan(&device, now_ms);
    begin_read_operation(&device, now_ms, response);

    // Cover the buffer out of order and repeat one range. Duplicates must not
    // hide a gap or prevent complete coverage.
    for (int offset = 560; offset >= 0; offset -= 70) {
        const size_t response_size = request_read_chunk(
            &device, now_ms, static_cast<uint16_t>(offset), response,
            offset == 0);
        assert(response_size > 11 && response[1] == 0x01);
        if (offset == 210) {
            assert(request_read_chunk(&device, now_ms, 210, response) ==
                   response_size);
            assert(response[1] == 0x01);
        }
    }
    assert(device.completed_reads == 0);
    assert(pa_device_read_in_progress(&device));
    assert(send(&device, now_ms++, 0x04, nullptr, 0, response) == 8);
    assert(response[1] == 0x04);
    assert(device.completed_reads == 1);
    assert(!pa_device_read_in_progress(&device));
    assert(send(&device, now_ms++, 0x04, nullptr, 0, response) == 8);
    assert(device.completed_reads == 1);

    // A partial read followed by Stop must not count.
    begin_read_operation(&device, now_ms, response);
    assert(request_read_chunk(&device, now_ms, 0, response) > 11);
    assert(request_read_chunk(&device, now_ms, 0, response) > 11);
    assert(send(&device, now_ms++, 0x04, nullptr, 0, response) == 8);
    assert(device.completed_reads == 1);

    // Full byte coverage still cannot count if the selected medium changes
    // before Stop closes the operation.
    begin_read_operation(&device, now_ms, response);
    for (uint16_t offset = 0; offset < NS2_NFC_READ_BUFFER_SIZE;
         offset = static_cast<uint16_t>(offset + NS2_NFC_READ_CHUNK_DATA_SIZE))
        assert(request_read_chunk(&device, now_ms, offset, response) > 11);
    uint8_t replacement[540]{};
    make_raw(replacement, 0x62);
    assert(pa_device_load_raw(&device, replacement, sizeof(replacement)));
    pa_device_present(&device, true);
    assert(send(&device, now_ms++, 0x04, nullptr, 0, response) == 8);
    assert(device.completed_reads == 1);

    // Out-of-range requests fail the operation and cannot be completed by a
    // later Stop.
    begin_read_operation(&device, now_ms, response);
    assert(request_read_chunk(&device, now_ms, 600, response) == 8);
    assert(!pa_device_read_in_progress(&device));
    assert(send(&device, now_ms++, 0x04, nullptr, 0, response) == 8);
    assert(device.completed_reads == 1);

    // The USB session reset clears runtime and transaction state while
    // retaining both lifetime counters.
    const uint32_t read_count = device.read_count;
    pa_device_reset_nfc(&device);
    assert(device.completed_reads == 1 && device.read_count == read_count);
    assert(!device.nfc.scan_active && !device.nfc.operation_active);
    assert(!pa_device_read_in_progress(&device));
}

static void test_swap_then_retry_read() {
    pa_device_t device{};
    pa_device_init(&device);
    uint8_t first[540]{};
    uint8_t second[540]{};
    make_raw(first, 0x23);
    make_raw(second, 0x59);
    second[7] ^= 1u;
    second[8] ^= 1u;
    assert(pa_device_load_raw(&device, first, sizeof(first)));
    pa_device_present(&device, true);
    uint32_t now_ms = 1;
    uint8_t response[128]{};
    start_scan(&device, now_ms);
    begin_read_operation(&device, now_ms, response);
    assert(request_read_chunk(&device, now_ms, 0, response) > 11);

    // Replacing the selected tag in the middle of a read invalidates the
    // transaction. The next chunk cannot finish the old buffer.
    assert(pa_device_load_raw(&device, second, sizeof(second)));
    pa_device_present(&device, true);
    assert(request_read_chunk(&device, now_ms, 70, response) == 8);
    assert(!pa_device_read_in_progress(&device));
    assert(device.completed_reads == 0);

    // Start a fresh read against the replacement image and complete it.
    start_scan(&device, now_ms);
    test_read_buffer(&device, now_ms, second);
    assert(send(&device, now_ms++, 0x04, nullptr, 0, response) == 8);
    assert(device.completed_reads == 1);
}

static void test_tag_changes_between_reads() {
    pa_device_t device{};
    pa_device_init(&device);
    uint8_t first[540]{};
    uint8_t second[540]{};
    make_raw(first, 0x27);
    make_raw(second, 0x55);
    second[7] ^= 1u;
    second[8] ^= 1u;
    uint32_t now_ms = 1;
    uint8_t response[128]{};

    assert(pa_device_load_raw(&device, first, sizeof(first)));
    pa_device_present(&device, true);
    start_scan(&device, now_ms);
    test_read_buffer(&device, now_ms, first);
    assert(send(&device, now_ms++, 0x04, nullptr, 0, response) == 8);
    pa_device_report(&device, now_ms++, nullptr, response);
    const uint8_t before_removal = response[12];
    pa_device_present(&device, false);
    pa_device_report(&device, now_ms++, nullptr, response);
    assert(response[12] == ((before_removal + 1u) & 7u));
    start_scan(&device, now_ms);
    pa_device_report(&device, now_ms++, nullptr, response);
    const uint8_t before_insert = response[12];

    assert(pa_device_load_raw(&device, second, sizeof(second)));
    pa_device_present(&device, true);
    pa_device_report(&device, now_ms++, nullptr, response);
    assert(response[12] == ((before_insert + 1u) & 7u));
    assert(device.nfc.scan_active);
    assert(send(&device, now_ms++, 0x05, nullptr, 0, response) == 69);
    assert(response[23] == second[7]);
    pa_device_report(&device, now_ms++, nullptr, response);
    assert(response[12] == ((before_insert + 1u) & 7u));
    test_read_buffer(&device, now_ms, second);
    assert(device.read_count == 18);

    // Replacing a placed card with Enter produces one new event and drops the
    // old operation. The next read must return the replacement's bytes.
    now_ms += 40;
    pa_device_report(&device, now_ms++, nullptr, response);
    const uint8_t before_replace = response[12];
    assert(pa_device_load_raw(&device, first, sizeof(first)));
    pa_device_present(&device, true);
    pa_device_report(&device, now_ms++, nullptr, response);
    assert(response[12] == ((before_replace + 1u) & 7u));
    assert(device.nfc.scan_active && !device.nfc.operation_active);
    test_read_buffer(&device, now_ms, first);
    assert(device.read_count == 27);
}

static void test_axes(pa_device_t *device) {
    uint8_t report[63]{};
    pa_device_report(device, 0, nullptr, report);
    const uint8_t centered[6] = {0, 8, 0x80, 0, 8, 0x80};
    assert(std::memcmp(report + 5, centered, sizeof(centered)) == 0);

    pa_device_set_sticks(device, 0x123, 0xABC, 0x1000, 0);
    pa_device_report(device, 1, nullptr, report);
    const uint8_t expected[6] = {0x23, 0xC1, 0xAB, 0xFF, 0x0F, 0x00};
    assert(std::memcmp(report + 5, expected, sizeof(expected)) == 0);
}

int main() {
    const uint8_t *descriptor = ns2_config_descriptor();
    assert(descriptor[2] == 80 && descriptor[3] == 0 && descriptor[4] == 2);
    assert(ns2_device_descriptor()[8] == 0x7E);
    assert(ns2_device_descriptor()[10] == 0x69);

    pa_device_t device{};
    pa_device_init(&device);
    test_axes(&device);

    uint8_t raw[540]{};
    make_raw(raw, 0x27);
    assert(pa_device_load_raw(&device, raw, sizeof(raw)));
    assert(device.loaded && !device.presented);
    pa_device_present(&device, true);
    assert(device.presented);

    uint8_t response[128]{};
    assert(send(&device, 0, 0x05, nullptr, 0, response) == 69);
    assert(response[8] == 0x09 && response[17] == raw[0]);
    uint32_t now_ms = 1;
    start_scan(&device, now_ms);
    test_read_buffer(&device, now_ms, raw);
    assert(device.read_count == 9);

    uint8_t extended[572]{};
    std::memcpy(extended, raw, sizeof(raw));
    for (size_t i = 0; i < 32; ++i)
        extended[540 + i] = static_cast<uint8_t>(0x80 + i);
    assert(pa_device_load_raw(&device, extended, sizeof(extended)));
    assert(device.loaded && !device.presented);
    assert(std::memcmp(device.signature, extended + 540, 32) == 0);
    pa_device_present(&device, true);
    start_scan(&device, now_ms);
    test_read_buffer(&device, now_ms, raw);
    assert(device.read_count == 18);

    uint8_t write_attempt[4] = {0x00, 0x00, 0x00, 0x00};
    assert(send(&device, now_ms++, 0x14, write_attempt,
                sizeof(write_attempt), response) == 8);
    assert(response[1] == 0x04);
    assert(send(&device, now_ms++, 0x08, nullptr, 0, response) == 8);
    assert(response[1] == 0x04);
    assert(std::memcmp(device.raw, raw, sizeof(device.raw)) == 0);
    assert(device.completed_reads == 0);

    assert(pa_device_load_raw(&device, device.raw, sizeof(device.raw)));
    assert(device.loaded && !device.presented);
    for (uint8_t byte : device.signature) assert(byte == 0);

    std::array<uint8_t, 540> invalid{};
    std::memcpy(invalid.data(), raw, sizeof(raw));
    invalid[3] ^= 1;
    assert(!pa_device_load_raw(&device, invalid.data(), invalid.size()));
    assert(!device.loaded && !device.presented);
    for (uint8_t byte : device.raw) assert(byte == 0);
    for (uint8_t byte : device.signature) assert(byte == 0);
    assert(!pa_device_load_raw(&device, raw, 539));
    assert(!device.loaded && !device.presented);

    test_tag_changes_between_reads();
    test_completed_read_tracking();
    test_swap_then_retry_read();
    std::cout << "PASS Switch 2 protocol core: descriptors, C++ API linkage, 540/572-byte RAM load, "
                 "read/present/chunks, whole-read completion, swap/retry, successive tag changes, "
                 "invalid-image clearing, stick packing\n";
}
"""


def main() -> int:
    gcc = shutil.which("gcc")
    gpp = shutil.which("g++")
    if not gcc or not gpp:
        print("ERROR: both gcc and g++ are required for the host protocol test", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="switch2-protocol-") as tmp:
        temp = pathlib.Path(tmp)
        objects: list[pathlib.Path] = []
        for source_name in SOURCES:
            source = ROOT / "src" / "ns2" / source_name
            obj = temp / f"{source.stem}.o"
            command = [
                gcc,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include" / "ns2"),
                "-c",
                str(source),
                "-o",
                str(obj),
            ]
            print("Compiling C:", source.relative_to(ROOT))
            result = subprocess.run(command, check=False)
            if result.returncode != 0:
                return result.returncode
            objects.append(obj)

        test_cpp = temp / "test_switch2_protocol_host.cpp"
        test_cpp.write_text(HOST_TEST, encoding="utf-8")
        test_object = temp / "test_switch2_protocol_host.o"
        compile_cpp = [
            gpp,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include"),
            "-c",
            str(test_cpp),
            "-o",
            str(test_object),
        ]
        print("Compiling C++ API smoke and regression test")
        result = subprocess.run(compile_cpp, check=False)
        if result.returncode != 0:
            return result.returncode

        executable = temp / "test_switch2_protocol_host.exe"
        link = [gpp, str(test_object), *map(str, objects), "-o", str(executable)]
        print("Linking C and C++ objects")
        result = subprocess.run(link, check=False)
        if result.returncode != 0:
            return result.returncode
        return subprocess.run([str(executable)], check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
