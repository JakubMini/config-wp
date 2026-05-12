/*****************************************************************************
 * Module:  test_tlv
 * Purpose: Generic TLV codec tests. Verifies wire-format byte layout,
 *          round-trip via writer + iterator, malformed-buffer rejection
 *          (truncated header, declared length walking off the end), and
 *          the emit-raw passthrough used to preserve unknown records.
 *****************************************************************************/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

extern "C"
{
#include "application/tlv.h"
}

/* --------------------------------------------------------------------- */
/* Writer                                                                */
/* --------------------------------------------------------------------- */

TEST(TlvWriter, EmitsWellFormedBytes)
{
    uint8_t      out[16] = {};
    tlv_writer_t w;
    tlv_writer_init(&w, out, sizeof(out));

    const uint8_t value[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    ASSERT_EQ(tlv_writer_emit(&w, 0xABCDu, value, sizeof(value)), TLV_OK);
    EXPECT_EQ(tlv_writer_size(&w), 4u + sizeof(value));

    /* tag (little-endian) */
    EXPECT_EQ(out[0], 0xCDu);
    EXPECT_EQ(out[1], 0xABu);
    /* length (little-endian) */
    EXPECT_EQ(out[2], 0x04u);
    EXPECT_EQ(out[3], 0x00u);
    /* value bytes */
    EXPECT_EQ(out[4], 0xDEu);
    EXPECT_EQ(out[5], 0xADu);
    EXPECT_EQ(out[6], 0xBEu);
    EXPECT_EQ(out[7], 0xEFu);
}

TEST(TlvWriter, AppendsRecordsSequentially)
{
    uint8_t      out[32] = {};
    tlv_writer_t w;
    tlv_writer_init(&w, out, sizeof(out));

    const uint8_t a[] = { 0x01 };
    const uint8_t b[] = { 0x02, 0x03 };
    ASSERT_EQ(tlv_writer_emit(&w, 0x0001u, a, sizeof(a)), TLV_OK);
    ASSERT_EQ(tlv_writer_emit(&w, 0x0002u, b, sizeof(b)), TLV_OK);
    EXPECT_EQ(tlv_writer_size(&w), 4u + 1u + 4u + 2u);
}

TEST(TlvWriter, NoSpaceRejected)
{
    uint8_t      out[6] = {}; /* only enough for one 2-byte record */
    tlv_writer_t w;
    tlv_writer_init(&w, out, sizeof(out));

    const uint8_t value[] = { 0xAA, 0xBB };
    ASSERT_EQ(tlv_writer_emit(&w, 0x0001u, value, sizeof(value)), TLV_OK);
    /* second record won't fit */
    EXPECT_EQ(tlv_writer_emit(&w, 0x0002u, value, sizeof(value)),
              TLV_ERR_NO_SPACE);
}

TEST(TlvWriter, NullValueWithLenRejected)
{
    uint8_t      out[16] = {};
    tlv_writer_t w;
    tlv_writer_init(&w, out, sizeof(out));
    EXPECT_EQ(tlv_writer_emit(&w, 0x0001u, nullptr, 4u), TLV_ERR_BUF);
}

TEST(TlvWriter, ZeroLengthValueIsLegal)
{
    /* A zero-length record (4-byte header, no payload) is a valid TLV. */
    uint8_t      out[16] = {};
    tlv_writer_t w;
    tlv_writer_init(&w, out, sizeof(out));
    ASSERT_EQ(tlv_writer_emit(&w, 0x1234u, nullptr, 0u), TLV_OK);
    EXPECT_EQ(tlv_writer_size(&w), 4u);
}

/* --------------------------------------------------------------------- */
/* Iterator                                                              */
/* --------------------------------------------------------------------- */

TEST(TlvIter, RoundTripWithWriter)
{
    uint8_t      out[64] = {};
    tlv_writer_t w;
    tlv_writer_init(&w, out, sizeof(out));

    const uint8_t a[] = { 0x11 };
    const uint8_t b[] = { 0x22, 0x33, 0x44 };
    ASSERT_EQ(tlv_writer_emit(&w, 0x0001u, a, sizeof(a)), TLV_OK);
    ASSERT_EQ(tlv_writer_emit(&w, 0x0002u, b, sizeof(b)), TLV_OK);

    tlv_iter_t it;
    tlv_iter_init(&it, out, tlv_writer_size(&w));

    uint16_t     tag       = 0;
    const void * value     = nullptr;
    size_t       value_len = 0;

    ASSERT_EQ(tlv_iter_next(&it, &tag, &value, &value_len), TLV_OK);
    EXPECT_EQ(tag, 0x0001u);
    EXPECT_EQ(value_len, sizeof(a));
    EXPECT_EQ(std::memcmp(value, a, sizeof(a)), 0);

    ASSERT_EQ(tlv_iter_next(&it, &tag, &value, &value_len), TLV_OK);
    EXPECT_EQ(tag, 0x0002u);
    EXPECT_EQ(value_len, sizeof(b));
    EXPECT_EQ(std::memcmp(value, b, sizeof(b)), 0);

    EXPECT_EQ(tlv_iter_next(&it, &tag, &value, &value_len), TLV_END);
    EXPECT_TRUE(tlv_iter_done(&it));
}

TEST(TlvIter, RejectsTruncatedHeader)
{
    /* 3 bytes — less than a TLV header (4 bytes) and we're past byte 0. */
    const uint8_t buf[] = { 0x01, 0x00, 0x04 };
    tlv_iter_t    it;
    tlv_iter_init(&it, buf, sizeof(buf));
    uint16_t     tag = 0;
    const void * v   = nullptr;
    size_t       vl  = 0;
    EXPECT_EQ(tlv_iter_next(&it, &tag, &v, &vl), TLV_ERR_TRUNCATED);
}

TEST(TlvIter, RejectsOverrunLength)
{
    /* Header declares a 10-byte payload but only 2 bytes follow. */
    const uint8_t buf[] = { 0x01, 0x00, 0x0A, 0x00, 0xFF, 0xFF };
    tlv_iter_t    it;
    tlv_iter_init(&it, buf, sizeof(buf));
    uint16_t     tag = 0;
    const void * v   = nullptr;
    size_t       vl  = 0;
    EXPECT_EQ(tlv_iter_next(&it, &tag, &v, &vl), TLV_ERR_OVERRUN);
}

TEST(TlvIter, EmptyBufferIsEndNotError)
{
    tlv_iter_t it;
    tlv_iter_init(&it, nullptr, 0);
    uint16_t     tag = 0;
    const void * v   = nullptr;
    size_t       vl  = 0;
    EXPECT_EQ(tlv_iter_next(&it, &tag, &v, &vl), TLV_END);
    EXPECT_TRUE(tlv_iter_done(&it));
}

/* --------------------------------------------------------------------- */
/* Raw passthrough (for unknown-record preservation)                     */
/* --------------------------------------------------------------------- */

TEST(TlvWriter, EmitRawCopiesAlreadyEncodedRecord)
{
    /* A pre-built record with tag 0x5555 and 2 value bytes */
    const uint8_t raw[]   = { 0x55, 0x55, 0x02, 0x00, 0xAB, 0xCD };
    uint8_t       out[16] = {};
    tlv_writer_t  w;
    tlv_writer_init(&w, out, sizeof(out));

    ASSERT_EQ(tlv_writer_emit_raw(&w, raw, sizeof(raw)), TLV_OK);
    EXPECT_EQ(tlv_writer_size(&w), sizeof(raw));
    EXPECT_EQ(std::memcmp(out, raw, sizeof(raw)), 0);
}

TEST(TlvWriter, EmitRawRejectsLengthMismatch)
{
    /* Declared length 4 but only 2 value bytes provided */
    const uint8_t bad[]   = { 0x01, 0x00, 0x04, 0x00, 0xAA, 0xBB };
    uint8_t       out[16] = {};
    tlv_writer_t  w;
    tlv_writer_init(&w, out, sizeof(out));
    EXPECT_EQ(tlv_writer_emit_raw(&w, bad, sizeof(bad)), TLV_ERR_OVERRUN);
}
