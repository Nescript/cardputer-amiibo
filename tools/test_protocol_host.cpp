#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "amiibo_tag.h"
#include "nfc_mcu.h"
#include "ntag215_emu.h"
#include "switch_protocol.h"

using namespace amiibo;

namespace {

void testCrc8() {
    uint8_t msg[33] = {0x01, 0x00, 0xFF, 0x00, 0x08, 0x00, 0x1B, 0x01};
    const uint8_t crc = mcuCrc8(msg, sizeof(msg));
    assert(crc == 0xC8);
}

void testAmiiboTagAndRandomUid() {
    AmiiboTag tag;
    tag.loadDemoBlankTag();
    assert(tag.isValid());
    assert(tag.verifyBcc());

    uint8_t uid[kUidSize]{};
    tag.getUid(uid);
    assert(uid[0] == 0x04);
    assert(uid[1] == 0x11);
    assert(uid[6] == 0x66);

    // Randomize UID with edge-case byte2 == 0x88 (must avoid 0x88)
    tag.randomizeUid(0x44338811U, 0x00006655U);
    assert(tag.verifyBcc());
    tag.getUid(uid);
    assert(uid[0] == 0x04);
    assert(uid[1] == 0x11);
    assert(uid[2] == 0x89);  // Cascade sentinel 0x88 prevented
    assert(uid[3] == 0x33);
    assert(uid[4] == 0x44);
    assert(uid[5] == 0x55);
    assert(uid[6] == 0x66);

    // Flipper .nfc text parsing
    std::string nfc_text =
        "Filetype: Flipper NFC device\n"
        "Version: 2\n"
        "Device type: NTAG215\n"
        "UID: 04 AA BB CC DD EE FF\n";
    for (int page = 0; page < 135; ++page) {
        char line[64];
        if (page == 0) {
            std::snprintf(line, sizeof(line), "Page 0: 04 AA BB 00\n");
        } else if (page == 1) {
            std::snprintf(line, sizeof(line), "Page 1: CC DD EE FF\n");
        } else if (page == 3) {
            std::snprintf(line, sizeof(line), "Page 3: E1 10 3E 00\n");
        } else {
            std::snprintf(line, sizeof(line), "Page %d: %02X 00 00 00\n", page, page & 0xFF);
        }
        nfc_text += line;
    }
    AmiiboTag parsed;
    assert(parsed.loadFlipperNfcText(nfc_text.data(), nfc_text.size()));
    assert(parsed.verifyBcc());
    parsed.getUid(uid);
    assert(uid[0] == 0x04 && uid[1] == 0xAA && uid[2] == 0xBB);
    assert(uid[3] == 0xCC && uid[4] == 0xDD && uid[5] == 0xEE && uid[6] == 0xFF);
    assert(parsed.data()[134 * 4] == 134);

    // 572-byte long dump loading truncates safely to 540
    std::vector<uint8_t> long_bin(kNtag215LongSize, 0xAB);
    long_bin[0] = 0x04;
    AmiiboTag long_tag;
    assert(long_tag.loadBin(long_bin.data(), long_bin.size()));
    assert(long_tag.verifyBcc());
    assert(long_tag.data()[539] == 0xAB);
}

void testWiredHandshakeAndSubcommands() {
    const uint8_t mac[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    SwitchProtocol proto(mac);
    OutgoingReport rep{};

    // On attach: 0x81 0x01 0x00 0x03 + reversed MAC
    proto.onAttach(rep);
    assert(rep.length == 64);
    assert(rep.data[0] == 0x81);
    assert(rep.data[1] == 0x01);
    assert(rep.data[3] == 0x03);
    assert(rep.data[4] == 0xBC && rep.data[9] == 0x12);
    assert(!proto.isReady());

    // 0x80 0x02 handshake
    uint8_t out_80_02[64] = {0x80, 0x02};
    assert(proto.onHostReport(out_80_02, sizeof(out_80_02), rep));
    assert(rep.data[0] == 0x81 && rep.data[1] == 0x02);

    // 0x80 0x03 baudrate
    uint8_t out_80_03[64] = {0x80, 0x03};
    assert(proto.onHostReport(out_80_03, sizeof(out_80_03), rep));
    assert(rep.data[0] == 0x81 && rep.data[1] == 0x03);

    // 0x80 0x04 enable USB HID
    uint8_t out_80_04[64] = {0x80, 0x04};
    assert(proto.onHostReport(out_80_04, sizeof(out_80_04), rep));
    assert(rep.data[0] == 0x81 && rep.data[1] == 0x04);
    assert(proto.isReady());

    // Subcommand 0x02 device info (forward MAC order!)
    uint8_t sub_02[64]{};
    sub_02[0] = 0x01;
    sub_02[10] = 0x02;
    assert(proto.onHostReport(sub_02, sizeof(sub_02), rep));
    assert(rep.length == 64);
    assert(rep.data[0] == 0x21);
    assert(rep.data[13] == 0x82 && rep.data[14] == 0x02);
    assert(rep.data[17] == 0x03);  // Pro Controller
    assert(rep.data[19] == 0x12 && rep.data[24] == 0xBC);

    // Subcommand 0x10 SPI read factory cal 0x6000 (16 bytes)
    uint8_t sub_10[64]{};
    sub_10[0] = 0x01;
    sub_10[10] = 0x10;
    sub_10[11] = 0x00;
    sub_10[12] = 0x60;
    sub_10[15] = 0x10;
    assert(proto.onHostReport(sub_10, sizeof(sub_10), rep));
    assert(rep.data[0] == 0x21 && rep.data[13] == 0x90 && rep.data[14] == 0x10);
    assert(rep.data[19] == 0x10);
    assert(rep.data[20] == 'C');

    // 0x80 0x92 UART tunnel wrapper containing subcommand 0x30 (set player lights)
    uint8_t uart_92[64]{};
    uart_92[0] = 0x80;
    uart_92[1] = 0x92;
    uart_92[8] = 0x01;
    uart_92[18] = 0x30;
    uart_92[19] = 0x03;  // P2 LEDs
    assert(proto.onHostReport(uart_92, sizeof(uart_92), rep));
    assert(rep.data[0] == 0x21 && rep.data[13] == 0x80 && rep.data[14] == 0x30);
    assert(proto.snapshot().player_leds == 0x03);
}

void testNfcFlowAnd362ByteTransfer() {
    SwitchProtocol proto;
    OutgoingReport rep{};
    proto.onAttach(rep);

    uint8_t out_80_04[64] = {0x80, 0x04};
    proto.onHostReport(out_80_04, sizeof(out_80_04), rep);

    // Switch sets 0x31 input report mode
    uint8_t sub_03_31[64]{};
    sub_03_31[0] = 0x01;
    sub_03_31[10] = 0x03;
    sub_03_31[11] = 0x31;
    assert(proto.onHostReport(sub_03_31, sizeof(sub_03_31), rep));
    assert(rep.data[13] == 0x80 && rep.data[14] == 0x03);
    assert(proto.inputMode() == 0x31);

    // Switch enables MCU (0x22 0x01)
    uint8_t sub_22[64]{};
    sub_22[0] = 0x01;
    sub_22[10] = 0x22;
    sub_22[11] = 0x01;
    assert(proto.onHostReport(sub_22, sizeof(sub_22), rep));
    assert(rep.data[13] == 0x80 && rep.data[14] == 0x22);

    // Switch configures MCU for NFC (0x21 0x21 0x00 0x04)
    uint8_t sub_21[64]{};
    sub_21[0] = 0x01;
    sub_21[10] = 0x21;
    sub_21[11] = 0x21;
    sub_21[12] = 0x00;
    sub_21[13] = 0x04;
    assert(proto.onHostReport(sub_21, sizeof(sub_21), rep));
    assert(rep.data[13] == 0xA0 && rep.data[14] == 0x21);
    assert(rep.data[22] == 0x04);
    assert(rep.data[48] == mcuCrc8(rep.data.data() + 15, 33));

    // Load recognizable 540-byte Amiibo dump and arm it
    std::vector<uint8_t> bin(kNtag215Size, 0);
    for (size_t i = 0; i < kNtag215Size; ++i) {
        bin[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFFU);
    }
    bin[0] = 0x04;
    bin[1] = 0x1A;
    bin[2] = 0x2B;
    bin[4] = 0x3C;
    bin[5] = 0x4D;
    bin[6] = 0x5E;
    bin[7] = 0x6F;
    AmiiboTag tag;
    assert(tag.loadBin(bin.data(), bin.size()));
    proto.mcu().setTag(tag, true);

    // Start polling: 0x11 [10]=0x02, [11]=0x01
    uint8_t mcu_start[64]{};
    mcu_start[0] = 0x11;
    mcu_start[10] = 0x02;
    mcu_start[11] = 0x01;
    assert(!proto.onHostReport(mcu_start, sizeof(mcu_start), rep));

    // Drain any queued status frames first
    for (int i = 0; i < 4; ++i) {
        assert(proto.buildStreamReport(rep));
        assert(rep.length == 362);
        assert(rep.data[0] == 0x31);
        assert(rep.data[361] == mcuCrc8(rep.data.data() + 49, 312));
    }

    // Poll NFC status: 0x11 [10]=0x02, [11]=0x04
    uint8_t mcu_poll[64]{};
    mcu_poll[0] = 0x11;
    mcu_poll[10] = 0x02;
    mcu_poll[11] = 0x04;
    proto.onHostReport(mcu_poll, sizeof(mcu_poll), rep);
    assert(proto.buildStreamReport(rep));
    assert(rep.length == 362);
    assert(rep.data[49] == 0x2A && rep.data[50] == 0x00 && rep.data[51] == 0x05);
    assert(rep.data[55] == 0x31 && rep.data[56] == 0x01);  // POLL
    assert(rep.data[62] == 0x02 && rep.data[64] == 0x07);  // NTAG + UID len 7
    assert(rep.data[65] == 0x04 && rep.data[71] == 0x6F);

    // Second poll transitions to POLL_AGAIN (0x09)
    proto.onHostReport(mcu_poll, sizeof(mcu_poll), rep);
    assert(proto.buildStreamReport(rep));
    assert(rep.data[56] == 0x09);

    // Read NTAG215: 0x11 [10]=0x02, [11]=0x06, UID bytes [18..24] == 0
    uint8_t mcu_read[64]{};
    mcu_read[0] = 0x11;
    mcu_read[10] = 0x02;
    mcu_read[11] = 0x06;
    mcu_read[17] = 0x07;
    proto.onHostReport(mcu_read, sizeof(mcu_read), rep);

    // Packet 1 (362B): bytes 0..244 at rep.data[116..360]
    OutgoingReport p1{}, p2{}, p3{};
    assert(proto.buildStreamReport(p1));
    assert(p1.length == 362);
    assert(p1.data[49] == 0x3A && p1.data[51] == 0x07 && p1.data[52] == 0x01);
    assert(p1.data[64] == 0x04 && p1.data[70] == 0x6F);  // UID
    assert(p1.data[74] == 0x00);                         // NTAG215 page marker for jctool
    assert(std::memcmp(p1.data.data() + 116, tag.data(), 245) == 0);
    assert(p1.data[361] == mcuCrc8(p1.data.data() + 49, 312));

    // Packet 2 (362B): bytes 245..539 at rep.data[56..350]
    assert(proto.buildStreamReport(p2));
    assert(p2.length == 362);
    assert(p2.data[49] == 0x3A && p2.data[51] == 0x07 && p2.data[52] == 0x02);
    assert(std::memcmp(p2.data.data() + 56, tag.data() + 245, 295) == 0);
    assert(p2.data[361] == mcuCrc8(p2.data.data() + 49, 312));

    // Packet 3 (362B): finished status 0x04
    assert(proto.buildStreamReport(p3));
    assert(p3.data[49] == 0x2A && p3.data[56] == 0x04);
    assert(p3.data[361] == mcuCrc8(p3.data.data() + 49, 312));
    assert(proto.snapshot().nfc.read_count == 1);

    // Verify NTAG write-back flow (0x06 with non-zero UID + 0x08 chunk)
    uint8_t mcu_write_setup[64]{};
    mcu_write_setup[0] = 0x11;
    mcu_write_setup[10] = 0x02;
    mcu_write_setup[11] = 0x06;
    mcu_write_setup[18] = 0x04;  // Non-zero UID triggers write setup
    proto.onHostReport(mcu_write_setup, sizeof(mcu_write_setup), rep);
    assert(proto.buildStreamReport(rep));
    assert(rep.data[49] == 0x3A && rep.data[55] == 0x40);
    assert(proto.mcu().nfcState() == NfcState::AwaitingWrite);

    uint8_t mcu_write_chunk[64]{};
    mcu_write_chunk[0] = 0x11;
    mcu_write_chunk[10] = 0x02;
    mcu_write_chunk[11] = 0x08;
    mcu_write_chunk[12] = 0x01;  // seq = 1
    mcu_write_chunk[14] = 0x08;  // end flag
    mcu_write_chunk[15] = 28;    // payload length
    // Write command payload starts at [16]
    mcu_write_chunk[16 + 1] = 0x07;  // UID len 7
    mcu_write_chunk[16 + 13] = 0xAA;
    mcu_write_chunk[16 + 17] = 0xBB;
    // Write page 20, len 4: DE AD BE EF
    mcu_write_chunk[16 + 22] = 20;
    mcu_write_chunk[16 + 23] = 4;
    mcu_write_chunk[16 + 24] = 0xDE;
    mcu_write_chunk[16 + 25] = 0xAD;
    mcu_write_chunk[16 + 26] = 0xBE;
    mcu_write_chunk[16 + 27] = 0xEF;
    proto.onHostReport(mcu_write_chunk, sizeof(mcu_write_chunk), rep);
    assert(proto.mcu().consumeWriteDirty());
    assert(proto.mcu().activeTag().data()[20 * 4 + 0] == 0xDE);
    assert(proto.mcu().activeTag().data()[20 * 4 + 3] == 0xEF);
    assert(proto.snapshot().nfc.write_count == 1);
}

void testButtonAndStickPacking() {
    SwitchProtocol proto;
    OutgoingReport rep{};
    proto.onAttach(rep);
    uint8_t out_80_04[64] = {0x80, 0x04};
    proto.onHostReport(out_80_04, sizeof(out_80_04), rep);

    ControllerState st{};
    st.a = true;
    st.r = true;
    st.home = true;
    st.dpad_up = true;
    st.l = true;
    st.lx = 0xABC;
    st.ly = 0x123;
    proto.setControllerState(st);

    assert(proto.buildStreamReport(rep));
    assert(rep.length == 64);
    assert(rep.data[0] == 0x30);
    assert(rep.data[2] == 0x91);
    assert(rep.data[3] == ((1U << 3) | (1U << 6)));  // A + R
    assert(rep.data[4] == (1U << 4));                // Home
    assert(rep.data[5] == ((1U << 1) | (1U << 6)));  // D-Pad Up + L
    assert(rep.data[6] == 0xBC);
    assert(rep.data[7] == 0x3A);
    assert(rep.data[8] == 0x12);
}

void testNtag215RfEmulation() {
    std::vector<uint8_t> long_bin(kNtag215LongSize, 0);
    for (size_t i = 0; i < kNtag215Size; ++i) {
        long_bin[i] = static_cast<uint8_t>((i * 5 + 1) & 0xFFU);
    }
    long_bin[0] = 0x04;
    long_bin[1] = 0x12;
    long_bin[2] = 0x34;
    long_bin[4] = 0x56;
    long_bin[5] = 0x78;
    long_bin[6] = 0x9A;
    long_bin[7] = 0xBC;
    // Set non-zero PWD & PACK in pages 133..134 to verify masking on READ/FAST_READ
    long_bin[133 * 4 + 0] = 0xDE;
    long_bin[133 * 4 + 1] = 0xAD;
    long_bin[133 * 4 + 2] = 0xBE;
    long_bin[133 * 4 + 3] = 0xEF;
    long_bin[134 * 4 + 0] = 0x80;
    long_bin[134 * 4 + 1] = 0x80;
    for (size_t i = 0; i < AmiiboTag::kSignatureSize; ++i) {
        long_bin[kNtag215Size + i] = static_cast<uint8_t>(0xA0 + i);
    }

    AmiiboTag tag;
    assert(tag.loadBin(long_bin.data(), long_bin.size()));
    assert(tag.hasSignature());
    assert(tag.signature()[0] == 0xA0 && tag.signature()[31] == 0xBF);

    Ntag215Emulator emu;
    emu.loadTag(tag);
    uint8_t out[560]{};

    // GET_VERSION (0x60)
    const uint8_t get_ver[1] = {0x60};
    RfReply r = emu.handleFrame(get_ver, sizeof(get_ver), out, sizeof(out));
    assert(!r.is_short_ack && r.length == 8);
    assert(out[1] == 0x04 && out[6] == 0x11 && out[7] == 0x03);

    // READ (0x30) page 0 + wraparound/PWD-mask check on page 132
    const uint8_t read_p0[2] = {0x30, 0x00};
    r = emu.handleFrame(read_p0, sizeof(read_p0), out, sizeof(out));
    assert(r.length == 16 && out[0] == 0x04 && out[1] == 0x12);

    const uint8_t read_p132[2] = {0x30, 132};
    r = emu.handleFrame(read_p132, sizeof(read_p132), out, sizeof(out));
    assert(r.length == 16);
    assert(std::memcmp(out, tag.data() + 132 * 4, 4) == 0);
    // Pages 133 and 134 (bytes 4..11) must be masked to 0x00; byte 12..15 wraps to page 0
    for (size_t i = 4; i < 12; ++i) {
        assert(out[i] == 0x00);
    }
    assert(out[12] == 0x04 && out[13] == 0x12);

    // FAST_READ (0x3A) full 0..134 (540 bytes), with pages 133..134 masked
    const uint8_t fast_all[3] = {0x3A, 0x00, 134};
    r = emu.handleFrame(fast_all, sizeof(fast_all), out, sizeof(out));
    assert(r.length == 540);
    assert(std::memcmp(out, tag.data(), 532) == 0);
    for (size_t i = 532; i < 540; ++i) {
        assert(out[i] == 0x00);
    }

    // Invalid FAST_READ range returns 4-bit NAK 0x00
    const uint8_t fast_bad[3] = {0x3A, 10, 5};
    r = emu.handleFrame(fast_bad, sizeof(fast_bad), out, sizeof(out));
    assert(r.is_short_ack && r.nibble == 0x00);

    // PWD_AUTH (0x1B) using Amiibo UID-derived password -> returns 2-byte PACK
    uint8_t uid[kUidSize]{};
    tag.getUid(uid);
    const uint8_t pwd_ok[5] = {
        0x1B,
        static_cast<uint8_t>(0xAAU ^ uid[1] ^ uid[3]),
        static_cast<uint8_t>(0x55U ^ uid[2] ^ uid[4]),
        static_cast<uint8_t>(0xAAU ^ uid[3] ^ uid[5]),
        static_cast<uint8_t>(0x55U ^ uid[4] ^ uid[6]),
    };
    r = emu.handleFrame(pwd_ok, sizeof(pwd_ok), out, sizeof(out));
    assert(!r.is_short_ack && r.length == 2 && out[0] == 0x80 && out[1] == 0x80);

    const uint8_t pwd_bad[5] = {0x1B, 0x00, 0x00, 0x00, 0x00};
    r = emu.handleFrame(pwd_bad, sizeof(pwd_bad), out, sizeof(out));
    assert(r.is_short_ack && r.nibble == 0x00);

    // WRITE (0xA2): protected page < 3 -> NAK 0x00; user page -> ACK 0x0A + dirty flag
    const uint8_t write_ro[6] = {0xA2, 0x01, 0x11, 0x22, 0x33, 0x44};
    r = emu.handleFrame(write_ro, sizeof(write_ro), out, sizeof(out));
    assert(r.is_short_ack && r.nibble == 0x00);
    assert(!emu.consumeWriteDirty());

    const uint8_t write_user[6] = {0xA2, 20, 0xCA, 0xFE, 0xBA, 0xBE};
    r = emu.handleFrame(write_user, sizeof(write_user), out, sizeof(out));
    assert(r.is_short_ack && r.nibble == 0x0A);
    assert(emu.consumeWriteDirty());
    assert(!emu.consumeWriteDirty());

    // COMP_WRITE (0xA0) 2-step flow
    const uint8_t comp_step1[2] = {0xA0, 21};
    r = emu.handleFrame(comp_step1, sizeof(comp_step1), out, sizeof(out));
    assert(r.is_short_ack && r.nibble == 0x0A);
    uint8_t comp_step2[16]{0x11, 0x22, 0x33, 0x44};
    r = emu.handleFrame(comp_step2, sizeof(comp_step2), out, sizeof(out));
    assert(r.is_short_ack && r.nibble == 0x0A);
    assert(emu.consumeWriteDirty());

    AmiiboTag exported;
    emu.exportTag(exported);
    assert(exported.data()[20 * 4 + 0] == 0xCA && exported.data()[20 * 4 + 3] == 0xBE);
    assert(exported.data()[21 * 4 + 0] == 0x11 && exported.data()[21 * 4 + 3] == 0x44);
    assert(exported.hasSignature() && exported.signature()[0] == 0xA0);

    // READ_SIG (0x3C) & READ_CNT (0x39)
    const uint8_t read_sig[2] = {0x3C, 0x00};
    r = emu.handleFrame(read_sig, sizeof(read_sig), out, sizeof(out));
    assert(r.length == 32 && out[0] == 0xA0 && out[31] == 0xBF);

    const uint8_t read_cnt[2] = {0x39, 0x02};
    r = emu.handleFrame(read_cnt, sizeof(read_cnt), out, sizeof(out));
    assert(r.length == 3 && out[0] == 0 && out[1] == 0 && out[2] == 0);

    // HLTA (0x50 0x00) triggers onFieldReset() -> completed_scans increments (>=4 pages read)
    const uint8_t hlta[2] = {0x50, 0x00};
    r = emu.handleFrame(hlta, sizeof(hlta), out, sizeof(out));
    assert(r.halt && r.length == 0 && !r.is_short_ack);
    assert(emu.stats().completed_scans == 1);
}

}  // namespace

int main() {
    testCrc8();
    testAmiiboTagAndRandomUid();
    testWiredHandshakeAndSubcommands();
    testNfcFlowAnd362ByteTransfer();
    testButtonAndStickPacking();
    testNtag215RfEmulation();
    std::printf("ALL HOST PROTOCOL TESTS PASSED!\n");
    return 0;
}
