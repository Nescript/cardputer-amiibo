/*
 * Amiibo key derivation and signing follow the format and cryptographic
 * algorithm established by amiitool:
 *   Copyright (c) 2015-2017 Marcos Del Sol Vives
 *   Copyright (c) 2016      javiMaD
 *   Copyright (c) 2016      Michael Armbruster
 *   https://github.com/socram8888/amiitool
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "amiibo_uid_signer.h"

#include <cstring>

#include <mbedtls/aes.h>
#include <mbedtls/md.h>

namespace amiibo {
namespace {

constexpr size_t kMasterKeyRecordSize = 80;
constexpr size_t kMasterKeyTypeStringOffset = 16;
constexpr size_t kMasterKeyTypeStringSize = 14;
constexpr size_t kMasterKeyMagicSizeOffset = 31;
constexpr size_t kMasterKeyMagicBytesOffset = 32;
constexpr size_t kMasterKeyXorPadOffset = 48;
constexpr size_t kSeedSize = 64;
constexpr size_t kPreparedSeedSize = 80;
constexpr size_t kDerivedKeySize = 16;

constexpr size_t kDataHmacOffset = 0x80;
constexpr size_t kTagHmacOffset = 0x34;
constexpr size_t kNtagEncryptedHeadOffset = 0x14;
constexpr size_t kNtagEncryptedHeadSize = 0x20;
constexpr size_t kNtagEncryptedTailOffset = 0xA0;
constexpr size_t kNtagEncryptedTailSize = 0x168;
constexpr size_t kNtagPasswordOffset = 0x214;
constexpr size_t kNtagPasswordAckOffset = 0x218;

void secureZero(void* memory, size_t size) {
    volatile uint8_t* cursor = static_cast<volatile uint8_t*>(memory);
    while (size-- > 0) {
        *cursor++ = 0;
    }
}

class WipeOnExit {
public:
    WipeOnExit(void* memory, size_t size) : memory_(memory), size_(size) {}
    ~WipeOnExit() { secureZero(memory_, size_); }

    WipeOnExit(const WipeOnExit&) = delete;
    WipeOnExit& operator=(const WipeOnExit&) = delete;

private:
    void* memory_;
    size_t size_;
};

struct DerivedKeys {
    uint8_t aes_key[kDerivedKeySize]{};
    uint8_t aes_iv[kDerivedKeySize]{};
    uint8_t hmac_key[kDerivedKeySize]{};

    ~DerivedKeys() { secureZero(this, sizeof(*this)); }
};

struct HmacPart {
    const uint8_t* data;
    size_t size;
};

bool hmacSha256(const uint8_t* key, size_t key_size, const HmacPart* parts,
                size_t part_count, uint8_t digest[32]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!key || !info || !parts || !digest) {
        return false;
    }

    mbedtls_md_context_t context;
    mbedtls_md_init(&context);

    int result = mbedtls_md_setup(&context, info, 1);
    if (result == 0) {
        result = mbedtls_md_hmac_starts(&context, key, key_size);
    }
    for (size_t i = 0; result == 0 && i < part_count; ++i) {
        if (parts[i].size > 0 && !parts[i].data) {
            result = -1;
        } else if (parts[i].size > 0) {
            result = mbedtls_md_hmac_update(&context, parts[i].data,
                                            parts[i].size);
        }
    }
    if (result == 0) {
        result = mbedtls_md_hmac_finish(&context, digest);
    }

    mbedtls_md_free(&context);
    return result == 0;
}

bool hasTypeStringTerminator(const uint8_t* record) {
    for (size_t i = 0; i < kMasterKeyTypeStringSize; ++i) {
        if (record[kMasterKeyTypeStringOffset + i] == 0) {
            return true;
        }
    }
    return false;
}

bool isMasterKeyRecordValid(const uint8_t* record) {
    return record && record[kMasterKeyTypeStringOffset] != 0 &&
           record[kMasterKeyMagicSizeOffset] <= 16 &&
           hasTypeStringTerminator(record);
}

bool deriveKeys(const uint8_t* master_key, const uint8_t* plain,
                DerivedKeys& derived) {
    if (!isMasterKeyRecordValid(master_key) || !plain) {
        return false;
    }

    uint8_t seed[kSeedSize]{};
    WipeOnExit seed_wiper(seed, sizeof(seed));
    seed[0] = plain[0x11];
    seed[1] = plain[0x12];
    std::memcpy(seed + 0x10, plain, 8);
    std::memcpy(seed + 0x18, plain, 8);
    std::memcpy(seed + 0x20, plain + 0x60, 0x20);

    uint8_t prepared_seed[kPreparedSeedSize]{};
    WipeOnExit prepared_seed_wiper(prepared_seed, sizeof(prepared_seed));
    size_t type_string_size = 0;
    while (type_string_size < kMasterKeyTypeStringSize &&
           master_key[kMasterKeyTypeStringOffset + type_string_size] != 0) {
        ++type_string_size;
    }
    if (type_string_size == kMasterKeyTypeStringSize) {
        return false;
    }

    size_t prepared_size = type_string_size + 1;
    std::memcpy(prepared_seed, master_key + kMasterKeyTypeStringOffset,
                prepared_size);

    const size_t magic_size = master_key[kMasterKeyMagicSizeOffset];
    const size_t seed_prefix_size = 16 - magic_size;
    std::memcpy(prepared_seed + prepared_size, seed, seed_prefix_size);
    prepared_size += seed_prefix_size;
    std::memcpy(prepared_seed + prepared_size,
                master_key + kMasterKeyMagicBytesOffset, magic_size);
    prepared_size += magic_size;
    std::memcpy(prepared_seed + prepared_size, seed + 0x10, 16);
    prepared_size += 16;
    for (size_t i = 0; i < 32; ++i) {
        prepared_seed[prepared_size + i] =
            seed[0x20 + i] ^ master_key[kMasterKeyXorPadOffset + i];
    }
    prepared_size += 32;

    uint8_t drbg_input[2 + kPreparedSeedSize]{};
    WipeOnExit drbg_input_wiper(drbg_input, sizeof(drbg_input));
    std::memcpy(drbg_input + 2, prepared_seed, prepared_size);

    uint8_t digest[32]{};
    WipeOnExit digest_wiper(digest, sizeof(digest));
    const HmacPart part{drbg_input, 2 + prepared_size};
    for (uint8_t counter = 0; counter < 2; ++counter) {
        drbg_input[0] = 0;
        drbg_input[1] = counter;
        if (!hmacSha256(master_key, kDerivedKeySize, &part, 1, digest)) {
            return false;
        }
        if (counter == 0) {
            std::memcpy(derived.aes_key, digest, kDerivedKeySize);
            std::memcpy(derived.aes_iv, digest + kDerivedKeySize,
                        kDerivedKeySize);
        } else {
            std::memcpy(derived.hmac_key, digest, kDerivedKeySize);
        }
    }
    return true;
}

bool cryptEncryptedFields(uint8_t* tag_data, const DerivedKeys& keys) {
    if (!tag_data) {
        return false;
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int result = mbedtls_aes_setkey_enc(&aes, keys.aes_key, 128);

    size_t nc_off = 0;
    uint8_t nonce_counter[16]{};
    uint8_t stream_block[16]{};
    WipeOnExit nonce_wiper(nonce_counter, sizeof(nonce_counter));
    WipeOnExit stream_wiper(stream_block, sizeof(stream_block));
    std::memcpy(nonce_counter, keys.aes_iv, sizeof(nonce_counter));

    if (result == 0) {
        result = mbedtls_aes_crypt_ctr(
            &aes, kNtagEncryptedHeadSize, &nc_off, nonce_counter, stream_block,
            tag_data + kNtagEncryptedHeadOffset,
            tag_data + kNtagEncryptedHeadOffset);
    }
    if (result == 0) {
        result = mbedtls_aes_crypt_ctr(
            &aes, kNtagEncryptedTailSize, &nc_off, nonce_counter, stream_block,
            tag_data + kNtagEncryptedTailOffset,
            tag_data + kNtagEncryptedTailOffset);
    }

    mbedtls_aes_free(&aes);
    return result == 0;
}

bool calculateTagHmac(const uint8_t* plain, const DerivedKeys& keys,
                       uint8_t digest[32]) {
    const HmacPart parts[] = {
        {plain, 8},
        {plain + 0x54, 0x2C},
    };
    return hmacSha256(keys.hmac_key, sizeof(keys.hmac_key), parts,
                      sizeof(parts) / sizeof(parts[0]), digest);
}

bool calculateDataHmac(const uint8_t* plain, const DerivedKeys& keys,
                       const uint8_t tag_hmac[32], uint8_t digest[32]) {
    const HmacPart parts[] = {
        {plain + 0x11, 0x23},
        {plain + 0xA0, 0x168},
        {tag_hmac, 32},
        {plain, 8},
        {plain + 0x54, 0x2C},
    };
    return hmacSha256(keys.hmac_key, sizeof(keys.hmac_key), parts,
                      sizeof(parts) / sizeof(parts[0]), digest);
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right, size_t size) {
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) {
        difference |= static_cast<uint8_t>(left[i] ^ right[i]);
    }
    return difference == 0;
}

bool verifyBcc(const uint8_t* tag_data) {
    const uint8_t bcc0 = static_cast<uint8_t>(0x88U ^ tag_data[0] ^
                                              tag_data[1] ^ tag_data[2]);
    const uint8_t bcc1 = static_cast<uint8_t>(tag_data[4] ^ tag_data[5] ^
                                              tag_data[6] ^ tag_data[7]);
    return tag_data[3] == bcc0 && tag_data[8] == bcc1;
}

void buildUid(uint32_t random0, uint32_t random1, uint8_t uid[7]) {
    uid[0] = 0x04;
    uid[1] = static_cast<uint8_t>(random0 & 0xFFU);
    uid[2] = static_cast<uint8_t>((random0 >> 8U) & 0xFFU);
    if (uid[2] == 0x88) {
        uid[2] = 0x89;
    }
    uid[3] = static_cast<uint8_t>((random0 >> 16U) & 0xFFU);
    uid[4] = static_cast<uint8_t>((random0 >> 24U) & 0xFFU);
    uid[5] = static_cast<uint8_t>(random1 & 0xFFU);
    uid[6] = static_cast<uint8_t>((random1 >> 8U) & 0xFFU);
}

void writeUidAndPassword(uint8_t* tag_data, const uint8_t uid[7]) {
    tag_data[0] = uid[0];
    tag_data[1] = uid[1];
    tag_data[2] = uid[2];
    tag_data[3] = static_cast<uint8_t>(0x88U ^ uid[0] ^ uid[1] ^ uid[2]);
    tag_data[4] = uid[3];
    tag_data[5] = uid[4];
    tag_data[6] = uid[5];
    tag_data[7] = uid[6];
    tag_data[8] = static_cast<uint8_t>(uid[3] ^ uid[4] ^ uid[5] ^ uid[6]);

    tag_data[kNtagPasswordOffset] =
        static_cast<uint8_t>(0xAAU ^ uid[1] ^ uid[3]);
    tag_data[kNtagPasswordOffset + 1] =
        static_cast<uint8_t>(0x55U ^ uid[2] ^ uid[4]);
    tag_data[kNtagPasswordOffset + 2] =
        static_cast<uint8_t>(0xAAU ^ uid[3] ^ uid[5]);
    tag_data[kNtagPasswordOffset + 3] =
        static_cast<uint8_t>(0x55U ^ uid[4] ^ uid[6]);
    tag_data[kNtagPasswordAckOffset] = 0x80;
    tag_data[kNtagPasswordAckOffset + 1] = 0x80;
}

}  // namespace

bool AmiiboUidSigner::loadKeys(const uint8_t* data, size_t size) {
    clear();
    if (!data || size != kKeySize ||
        !isMasterKeyRecordValid(data) ||
        !isMasterKeyRecordValid(data + kMasterKeyRecordSize)) {
        return false;
    }

    std::memcpy(keys_.data(), data, kKeySize);
    ready_ = true;
    return true;
}

void AmiiboUidSigner::clear() {
    secureZero(keys_.data(), keys_.size());
    ready_ = false;
}

bool AmiiboUidSigner::resign(AmiiboTag& tag, uint32_t random0,
                             uint32_t random1) const {
    if (!ready_ || !tag.isValid()) {
        return false;
    }

    std::array<uint8_t, kNtag215Size> working{};
    WipeOnExit working_wiper(working.data(), working.size());
    std::memcpy(working.data(), tag.data(), working.size());
    if (working[0] != 0x04 || !verifyBcc(working.data())) {
        return false;
    }

    DerivedKeys original_data_keys;
    DerivedKeys original_tag_keys;
    if (!deriveKeys(keys_.data(), working.data(), original_data_keys) ||
        !deriveKeys(keys_.data() + kMasterKeyRecordSize, working.data(),
                    original_tag_keys) ||
        !cryptEncryptedFields(working.data(), original_data_keys)) {
        return false;
    }

    uint8_t original_tag_hmac[32]{};
    WipeOnExit original_tag_wiper(original_tag_hmac, sizeof(original_tag_hmac));
    uint8_t original_data_hmac[32]{};
    WipeOnExit original_data_wiper(original_data_hmac,
                                   sizeof(original_data_hmac));
    if (!calculateTagHmac(working.data(), original_tag_keys,
                          original_tag_hmac) ||
        !calculateDataHmac(working.data(), original_data_keys,
                           original_tag_hmac, original_data_hmac) ||
        !constantTimeEqual(original_tag_hmac,
                           working.data() + kTagHmacOffset,
                           sizeof(original_tag_hmac)) ||
        !constantTimeEqual(original_data_hmac,
                           working.data() + kDataHmacOffset,
                           sizeof(original_data_hmac))) {
        return false;
    }

    uint8_t uid[7]{};
    WipeOnExit uid_wiper(uid, sizeof(uid));
    buildUid(random0, random1, uid);
    if (uid[0] == working[0] && uid[1] == working[1] &&
        uid[2] == working[2] && uid[3] == working[4] &&
        uid[4] == working[5] && uid[5] == working[6] &&
        uid[6] == working[7]) {
        return false;
    }

    writeUidAndPassword(working.data(), uid);

    DerivedKeys new_data_keys;
    DerivedKeys new_tag_keys;
    if (!deriveKeys(keys_.data(), working.data(), new_data_keys) ||
        !deriveKeys(keys_.data() + kMasterKeyRecordSize, working.data(),
                    new_tag_keys)) {
        return false;
    }

    uint8_t new_tag_hmac[32]{};
    WipeOnExit new_tag_wiper(new_tag_hmac, sizeof(new_tag_hmac));
    uint8_t new_data_hmac[32]{};
    WipeOnExit new_data_wiper(new_data_hmac, sizeof(new_data_hmac));
    if (!calculateTagHmac(working.data(), new_tag_keys, new_tag_hmac) ||
        !calculateDataHmac(working.data(), new_data_keys, new_tag_hmac,
                           new_data_hmac)) {
        return false;
    }

    std::memcpy(working.data() + kTagHmacOffset, new_tag_hmac,
                sizeof(new_tag_hmac));
    std::memcpy(working.data() + kDataHmacOffset, new_data_hmac,
                sizeof(new_data_hmac));
    if (!cryptEncryptedFields(working.data(), new_data_keys)) {
        return false;
    }

    std::memcpy(tag.mutableData(), working.data(), working.size());
    return true;
}

}  // namespace amiibo
