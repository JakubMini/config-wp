/*****************************************************************************
 * Module:  test_crc32
 * Purpose: Verify the CRC-32 implementation against the canonical test
 *          vectors used by zlib / RFC-1952. If these numbers ever change,
 *          something is wrong with the polynomial, the reflection, or the
 *          final XOR.
 *****************************************************************************/

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

extern "C"
{
#include "application/crc32.h"
}

TEST(Crc32, EmptyInputIsZero)
{
    EXPECT_EQ(crc32_compute(nullptr, 0), 0x00000000u);
}

TEST(Crc32, StandardVector_123456789)
{
    /* Canonical CRC-32/ISO-HDLC test vector. Verified independently with
     *   python -c "import binascii;print(hex(binascii.crc32(b'123456789')))"
     *   -> 0xcbf43926
     */
    const char * msg = "123456789";
    EXPECT_EQ(crc32_compute(msg, std::strlen(msg)), 0xCBF43926u);
}

TEST(Crc32, SingleZeroByte)
{
    const uint8_t zero = 0u;
    /* python -c "import binascii;print(hex(binascii.crc32(b'\\x00')))"
     * -> 0xd202ef8d */
    EXPECT_EQ(crc32_compute(&zero, 1), 0xD202EF8Du);
}

TEST(Crc32, StreamingMatchesOneShot)
{
    const char * a = "12345";
    const char * b = "6789";

    const uint32_t one_shot = crc32_compute("123456789", 9);

    uint32_t state          = crc32_start();
    state                   = crc32_step(state, a, std::strlen(a));
    state                   = crc32_step(state, b, std::strlen(b));
    const uint32_t streamed = crc32_end(state);

    EXPECT_EQ(one_shot, streamed);
    EXPECT_EQ(streamed, 0xCBF43926u);
}

TEST(Crc32, InitIsIdempotent)
{
    /* crc32_init builds the lookup table; calling it twice must produce
     * the same answers afterwards. */
    crc32_init();
    crc32_init();
    EXPECT_EQ(crc32_compute("123456789", 9), 0xCBF43926u);
}
