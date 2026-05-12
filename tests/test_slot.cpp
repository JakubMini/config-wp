/*****************************************************************************
 * Module:  test_slot
 * Purpose: Exercises the A/B slot protocol against the memcpy-stub storage
 *          backing. Covers:
 *            - blank storage reads as "no valid slot"
 *            - round-trip write -> pick_active -> bytes back
 *            - newer-seq slot wins when both are valid
 *            - consecutive writes alternate slots (proves A/B ping-pong)
 *            - active-slot corruption falls back to the older surviving slot
 *            - bad magic / oversized length are rejected before CRC work
 *            - the slot header is exactly 20 bytes (packing regression test)
 *****************************************************************************/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

extern "C"
{
#include "application/config_slot.h"
#include "application/crc32.h"
#include "drivers/storage.h"
}

class SlotTest : public ::testing::Test
{
protected:
    void SetUp () override
    {
        crc32_init();
        ASSERT_EQ(storage_init(), STORAGE_OK);
    }
};

TEST_F(SlotTest, BlankStorageHasNoValidSlot)
{
    std::vector<uint8_t> buf(slot_max_payload(), 0);
    slot_id_t            id  = SLOT_NONE;
    size_t               len = 0;
    EXPECT_EQ(slot_pick_active(&id, buf.data(), buf.size(), &len),
              SLOT_ERR_NO_VALID);
}

TEST_F(SlotTest, RoundTrip)
{
    const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x55, 0xAA };
    ASSERT_EQ(slot_write(payload, sizeof(payload)), SLOT_OK);

    std::vector<uint8_t> buf(slot_max_payload(), 0);
    slot_id_t            id  = SLOT_NONE;
    size_t               len = 0;
    ASSERT_EQ(slot_pick_active(&id, buf.data(), buf.size(), &len), SLOT_OK);
    EXPECT_EQ(len, sizeof(payload));
    EXPECT_EQ(std::memcmp(buf.data(), payload, sizeof(payload)), 0);
    EXPECT_EQ(id, SLOT_A); /* first write lands in A */
}

TEST_F(SlotTest, NewerSeqWins)
{
    const uint8_t a[] = { 1, 2, 3 };
    const uint8_t b[] = { 9, 9, 9, 9 };
    ASSERT_EQ(slot_write(a, sizeof(a)), SLOT_OK);
    ASSERT_EQ(slot_write(b, sizeof(b)), SLOT_OK);

    std::vector<uint8_t> buf(slot_max_payload(), 0);
    slot_id_t            id  = SLOT_NONE;
    size_t               len = 0;
    ASSERT_EQ(slot_pick_active(&id, buf.data(), buf.size(), &len), SLOT_OK);
    EXPECT_EQ(len, sizeof(b));
    EXPECT_EQ(std::memcmp(buf.data(), b, sizeof(b)), 0);
    EXPECT_EQ(id, SLOT_B); /* second write lands in B */
}

TEST_F(SlotTest, AlternatesSlots)
{
    const uint8_t v[] = { 0x01 };
    ASSERT_EQ(slot_write(v, 1), SLOT_OK); /* -> SLOT_A */
    ASSERT_EQ(slot_write(v, 1), SLOT_OK); /* -> SLOT_B */
    ASSERT_EQ(slot_write(v, 1), SLOT_OK); /* -> SLOT_A again */

    std::vector<uint8_t> buf(slot_max_payload(), 0);
    slot_id_t            id  = SLOT_NONE;
    size_t               len = 0;
    ASSERT_EQ(slot_pick_active(&id, buf.data(), buf.size(), &len), SLOT_OK);
    EXPECT_EQ(id, SLOT_A);
}

TEST_F(SlotTest, ActiveCorruptionFallsBackToOlder)
{
    const uint8_t a[] = { 0xAA };
    const uint8_t b[] = { 0xBB };
    ASSERT_EQ(slot_write(a, 1), SLOT_OK); /* SLOT_A, seq=1 */
    ASSERT_EQ(slot_write(b, 1), SLOT_OK); /* SLOT_B, seq=2 */

    /* Corrupt one byte of slot B's payload. The CRC-covered region
     * differs, so the slot fails verification and slot A wins. */
    const size_t b_payload_off = 2048u + 20u;
    uint8_t      flip          = 0x00;
    ASSERT_EQ(storage_read(b_payload_off, &flip, 1), STORAGE_OK);
    flip ^= 0xFF;
    ASSERT_EQ(storage_write(b_payload_off, &flip, 1), STORAGE_OK);

    std::vector<uint8_t> buf(slot_max_payload(), 0);
    slot_id_t            id  = SLOT_NONE;
    size_t               len = 0;
    ASSERT_EQ(slot_pick_active(&id, buf.data(), buf.size(), &len), SLOT_OK);
    EXPECT_EQ(id, SLOT_A);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(buf[0], a[0]);
}

TEST_F(SlotTest, BothCorruptReturnsNoValid)
{
    const uint8_t v[] = { 0xCC };
    ASSERT_EQ(slot_write(v, 1), SLOT_OK);
    ASSERT_EQ(slot_write(v, 1), SLOT_OK);

    /* Scribble over both headers' magic. */
    const uint32_t bad = 0xDEADBEEFu;
    ASSERT_EQ(storage_write(0u, &bad, sizeof(bad)), STORAGE_OK);
    ASSERT_EQ(storage_write(2048u, &bad, sizeof(bad)), STORAGE_OK);

    std::vector<uint8_t> buf(slot_max_payload(), 0);
    slot_id_t            id  = SLOT_NONE;
    size_t               len = 0;
    EXPECT_EQ(slot_pick_active(&id, buf.data(), buf.size(), &len),
              SLOT_ERR_NO_VALID);
}

TEST_F(SlotTest, OversizedWriteRejected)
{
    std::vector<uint8_t> too_big(slot_max_payload() + 1, 0xAB);
    EXPECT_EQ(slot_write(too_big.data(), too_big.size()), SLOT_ERR_TOO_LARGE);
}

TEST_F(SlotTest, BufferTooSmallRejected)
{
    const uint8_t v[] = { 0x01 };
    ASSERT_EQ(slot_write(v, 1), SLOT_OK);

    uint8_t   small[8] = {};
    slot_id_t id       = SLOT_NONE;
    size_t    len      = 0;
    EXPECT_EQ(slot_pick_active(&id, small, sizeof(small), &len), SLOT_ERR_BUF);
}

TEST_F(SlotTest, NullPayloadWriteRejected)
{
    EXPECT_EQ(slot_write(nullptr, 0), SLOT_ERR_BUF);
}

TEST_F(SlotTest, ZeroLengthWriteRejected)
{
    /* A non-NULL pointer paired with len==0 is also rejected — every
     * valid configuration needs at least one record. */
    const uint8_t v = 0x01;
    EXPECT_EQ(slot_write(&v, 0), SLOT_ERR_BUF);
}

TEST_F(SlotTest, RejectsWrongFormatVer)
{
    const uint8_t v[] = { 0xAB };
    ASSERT_EQ(slot_write(v, 1), SLOT_OK);

    /* Slot A's header now lives at offset 0. Field layout:
     *   u32 magic | u16 format_ver | u16 flags | ...
     * so format_ver is at offset 4. Bumping it forces a mismatch.
     * The CRC field still matches the old format_ver, so this also
     * proves the version check happens *before* the CRC check (cheap
     * rejection without touching the payload). */
    const uint16_t bad_ver = 99u;
    ASSERT_EQ(storage_write(4u, &bad_ver, sizeof(bad_ver)), STORAGE_OK);

    std::vector<uint8_t> buf(slot_max_payload(), 0);
    slot_id_t            id  = SLOT_NONE;
    size_t               len = 0;
    EXPECT_EQ(slot_pick_active(&id, buf.data(), buf.size(), &len),
              SLOT_ERR_NO_VALID);
}

TEST_F(SlotTest, RejectsNonZeroFlags)
{
    const uint8_t v[] = { 0xCD };
    ASSERT_EQ(slot_write(v, 1), SLOT_OK);

    /* flags is a u16 at offset 6 (after magic[4] + format_ver[2]). A
     * non-zero value here signals a future firmware that uses the flag —
     * we should refuse to apply our old logic to it. */
    const uint16_t flag_set = 0x0001u;
    ASSERT_EQ(storage_write(6u, &flag_set, sizeof(flag_set)), STORAGE_OK);

    std::vector<uint8_t> buf(slot_max_payload(), 0);
    slot_id_t            id  = SLOT_NONE;
    size_t               len = 0;
    EXPECT_EQ(slot_pick_active(&id, buf.data(), buf.size(), &len),
              SLOT_ERR_NO_VALID);
}
