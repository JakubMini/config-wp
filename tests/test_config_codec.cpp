/*****************************************************************************
 * Module:  test_config_codec
 * Purpose: Round-trips every IO struct and the system struct through
 *          their wire encode/decode. Pins wire sizes against the declared
 *          constants. Demonstrates the unknown-tag-preserved-on-rewrite
 *          property by mixing a synthetic unknown record into a stream
 *          built from known records and verifying that re-encoding via
 *          tlv_writer_emit_raw passes it through byte-for-byte.
 *****************************************************************************/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

extern "C"
{
#include "application/config_codec.h"
#include "application/config_defaults.h"
#include "application/config_types.h"
#include "application/tlv.h"
}

namespace {

/*
 * Round-trip a struct through encode -> decode -> re-encode and assert
 * the re-encoded byte stream matches the original byte stream.
 *
 * Comparing re-encoded BYTES rather than the raw struct memory avoids
 * portability issues with struct padding: C struct assignment is
 * allowed to leave padding bytes undefined, and `Struct out = {}`
 * zero-fills padding — so memcmp(&in, &out, sizeof(Struct)) can spuriously
 * fail across toolchains even when every field round-trips correctly.
 *
 * The byte-level check is also stronger: it pins the on-wire
 * representation, which is the contract callers actually depend on.
 */
template<typename Encoder, typename Decoder, typename Struct>
void
round_trip (Encoder enc, Decoder dec, const Struct & in, size_t expected_size)
{
    uint8_t buf_a[64] = {};
    size_t  size_a    = 0;
    ASSERT_EQ(enc(&in, buf_a, sizeof(buf_a), &size_a), TLV_OK);
    EXPECT_EQ(size_a, expected_size);

    Struct decoded = {};
    ASSERT_EQ(dec(buf_a, size_a, &decoded), TLV_OK);

    uint8_t buf_b[64] = {};
    size_t  size_b    = 0;
    ASSERT_EQ(enc(&decoded, buf_b, sizeof(buf_b), &size_b), TLV_OK);
    EXPECT_EQ(size_b, size_a);
    EXPECT_EQ(std::memcmp(buf_a, buf_b, size_a), 0)
        << "encode -> decode -> encode must reproduce the wire bytes";
}

} // namespace

/* --------------------------------------------------------------------- */
/* Wire-size invariants                                                  */
/* --------------------------------------------------------------------- */

TEST(ConfigCodecSizes, MatchDeclaredConstants)
{
    uint8_t buf[64] = {};
    size_t  size    = 0;

    ASSERT_EQ(
        config_codec_encode_di(&g_di_defaults[0], buf, sizeof(buf), &size),
        TLV_OK);
    EXPECT_EQ(size, CONFIG_CODEC_DI_WIRE_SIZE);

    ASSERT_EQ(
        config_codec_encode_do(&g_do_defaults[0], buf, sizeof(buf), &size),
        TLV_OK);
    EXPECT_EQ(size, CONFIG_CODEC_DO_WIRE_SIZE);

    ASSERT_EQ(
        config_codec_encode_tc(&g_tc_defaults[0], buf, sizeof(buf), &size),
        TLV_OK);
    EXPECT_EQ(size, CONFIG_CODEC_TC_WIRE_SIZE);

    ASSERT_EQ(
        config_codec_encode_ai(&g_ai_defaults[0], buf, sizeof(buf), &size),
        TLV_OK);
    EXPECT_EQ(size, CONFIG_CODEC_AI_WIRE_SIZE);

    ASSERT_EQ(
        config_codec_encode_ao(&g_ao_defaults[0], buf, sizeof(buf), &size),
        TLV_OK);
    EXPECT_EQ(size, CONFIG_CODEC_AO_WIRE_SIZE);

    ASSERT_EQ(
        config_codec_encode_pcnt(&g_pcnt_defaults[0], buf, sizeof(buf), &size),
        TLV_OK);
    EXPECT_EQ(size, CONFIG_CODEC_PCNT_WIRE_SIZE);

    ASSERT_EQ(
        config_codec_encode_pwm(&g_pwm_defaults[0], buf, sizeof(buf), &size),
        TLV_OK);
    EXPECT_EQ(size, CONFIG_CODEC_PWM_WIRE_SIZE);

    ASSERT_EQ(
        config_codec_encode_system(&g_system_defaults, buf, sizeof(buf), &size),
        TLV_OK);
    EXPECT_EQ(size, CONFIG_CODEC_SYSTEM_WIRE_SIZE);
}

/* --------------------------------------------------------------------- */
/* Tag construction / parsing                                            */
/* --------------------------------------------------------------------- */

TEST(ConfigCodecTags, ConstructAndParse)
{
    const uint16_t tag = config_codec_make_tag(CONFIG_CODEC_DOMAIN_DI, 5u);
    EXPECT_EQ(config_codec_tag_domain(tag), CONFIG_CODEC_DOMAIN_DI);
    EXPECT_EQ(config_codec_tag_index(tag), 5u);

    const uint16_t sys = config_codec_make_tag(CONFIG_CODEC_DOMAIN_SYSTEM,
                                               CONFIG_CODEC_SYSTEM_INDEX);
    EXPECT_EQ(config_codec_tag_domain(sys), CONFIG_CODEC_DOMAIN_SYSTEM);
    EXPECT_EQ(config_codec_tag_index(sys), 0u);
}

/* --------------------------------------------------------------------- */
/* Per-type round-trips with non-default values                          */
/* --------------------------------------------------------------------- */

TEST(ConfigCodecRoundTrip, Di)
{
    di_config_t in = g_di_defaults[0];
    std::strncpy(in.name, "front_door", CONFIG_NAME_LEN - 1);
    in.id                = 0x1234;
    in.debounce_ms       = 42;
    in.polarity          = DI_POLARITY_ACTIVE_LOW;
    in.fault_state       = FAULT_STATE_HIGH;
    in.interrupt_enabled = true;
    round_trip(config_codec_encode_di,
               config_codec_decode_di,
               in,
               CONFIG_CODEC_DI_WIRE_SIZE);
}

TEST(ConfigCodecRoundTrip, Do)
{
    do_config_t in = g_do_defaults[0];
    std::strncpy(in.name, "valve_1", CONFIG_NAME_LEN - 1);
    in.id          = 0x5678;
    in.polarity    = DO_POLARITY_ACTIVE_LOW;
    in.fault_state = FAULT_STATE_HIGH;
    round_trip(config_codec_encode_do,
               config_codec_decode_do,
               in,
               CONFIG_CODEC_DO_WIRE_SIZE);
}

TEST(ConfigCodecRoundTrip, Tc)
{
    tc_config_t in = g_tc_defaults[0];
    std::strncpy(in.name, "exhaust_temp", CONFIG_NAME_LEN - 1);
    in.id              = 0xAAAA;
    in.tc_type         = TC_TYPE_J;
    in.unit            = TC_UNIT_FAHRENHEIT;
    in.cjc_enabled     = false;
    in.filter_ms       = 250;
    in.fault_state     = FAULT_STATE_HOLD;
    in.fault_value_c10 = -1234;
    round_trip(config_codec_encode_tc,
               config_codec_decode_tc,
               in,
               CONFIG_CODEC_TC_WIRE_SIZE);
}

TEST(ConfigCodecRoundTrip, Ai)
{
    ai_config_t in = g_ai_defaults[0];
    std::strncpy(in.name, "pressure_in", CONFIG_NAME_LEN - 1);
    in.id          = 0xBEEF;
    in.input_mode  = AI_INPUT_MODE_CURRENT_4_20MA_3W;
    in.filter_ms   = 100;
    in.scale_num   = 1000;
    in.scale_den   = 4096;
    in.offset      = -50;
    in.fault_state = FAULT_STATE_HIGH;
    in.fault_value = 9999;
    round_trip(config_codec_encode_ai,
               config_codec_decode_ai,
               in,
               CONFIG_CODEC_AI_WIRE_SIZE);
}

TEST(ConfigCodecRoundTrip, Ao)
{
    ao_config_t in = g_ao_defaults[0];
    std::strncpy(in.name, "flow_setpoint", CONFIG_NAME_LEN - 1);
    in.id          = 0xCAFE;
    in.output_mode = AO_OUTPUT_MODE_CURRENT_4_20MA;
    in.slew_per_s  = 500;
    in.scale_num   = 10;
    in.scale_den   = 4;
    in.offset      = 1;
    in.fault_state = FAULT_STATE_LOW;
    in.fault_value = -42;
    round_trip(config_codec_encode_ao,
               config_codec_decode_ao,
               in,
               CONFIG_CODEC_AO_WIRE_SIZE);
}

TEST(ConfigCodecRoundTrip, Pcnt)
{
    pcnt_config_t in = g_pcnt_defaults[0];
    std::strncpy(in.name, "wheel_tick", CONFIG_NAME_LEN - 1);
    in.id            = 0x0007;
    in.mode          = PCNT_MODE_FREQUENCY;
    in.edge          = PCNT_EDGE_BOTH;
    in.limit         = 1000000u;
    in.reset_on_read = true;
    round_trip(config_codec_encode_pcnt,
               config_codec_decode_pcnt,
               in,
               CONFIG_CODEC_PCNT_WIRE_SIZE);
}

TEST(ConfigCodecRoundTrip, Pwm)
{
    pwm_config_t in = g_pwm_defaults[0];
    std::strncpy(in.name, "fan_pwm", CONFIG_NAME_LEN - 1);
    in.id                  = 0x4242;
    in.period_us           = 5000;
    in.duty_permille       = 750;
    in.fault_state         = FAULT_STATE_HOLD;
    in.fault_duty_permille = 250;
    round_trip(config_codec_encode_pwm,
               config_codec_decode_pwm,
               in,
               CONFIG_CODEC_PWM_WIRE_SIZE);
}

TEST(ConfigCodecRoundTrip, System)
{
    system_config_t in      = g_system_defaults;
    in.canopen_node_id      = 42;
    in.can_bitrate          = CAN_BITRATE_1M;
    in.heartbeat_ms         = 250;
    in.sync_window_us       = 100;
    in.nmt_startup          = NMT_STARTUP_AUTOSTART;
    in.producer_emcy_cob_id = 0x0123;
    round_trip(config_codec_encode_system,
               config_codec_decode_system,
               in,
               CONFIG_CODEC_SYSTEM_WIRE_SIZE);
}

/* --------------------------------------------------------------------- */
/* Decode error paths                                                    */
/* --------------------------------------------------------------------- */

TEST(ConfigCodecDecode, TruncatedBufferRejected)
{
    uint8_t buf[CONFIG_CODEC_DI_WIRE_SIZE] = {};
    size_t  size                           = 0;
    ASSERT_EQ(
        config_codec_encode_di(&g_di_defaults[0], buf, sizeof(buf), &size),
        TLV_OK);

    di_config_t out = {};
    EXPECT_EQ(config_codec_decode_di(buf, size - 1, &out), TLV_ERR_TRUNCATED);
}

TEST(ConfigCodecEncode, NullOutSizeRejected)
{
    /* Every encode_* writes to *out_size unconditionally on success.
     * A NULL out_size must be rejected up front rather than
     * dereferenced. */
    uint8_t buf[64] = {};
    EXPECT_EQ(
        config_codec_encode_di(&g_di_defaults[0], buf, sizeof(buf), nullptr),
        TLV_ERR_BUF);
    EXPECT_EQ(config_codec_encode_system(
                  &g_system_defaults, buf, sizeof(buf), nullptr),
              TLV_ERR_BUF);
}

TEST(ConfigCodecDecode, NameAlwaysNullTerminated)
{
    /* Forge a wire blob where every name byte is non-zero. The decoder
     * must still produce a null-terminated string. */
    uint8_t buf[CONFIG_CODEC_DI_WIRE_SIZE] = {};
    std::memset(buf, 'A', CONFIG_NAME_LEN);
    /* fill the remaining fields with arbitrary in-range bytes */
    buf[CONFIG_NAME_LEN]     = 0x01;
    buf[CONFIG_NAME_LEN + 1] = 0x00;
    buf[CONFIG_NAME_LEN + 2] = 0x0A;
    buf[CONFIG_NAME_LEN + 3] = 0x00;
    /* polarity, fault_state, interrupt_enabled: 0,0,0 */

    di_config_t out = {};
    ASSERT_EQ(config_codec_decode_di(buf, sizeof(buf), &out), TLV_OK);
    EXPECT_EQ(out.name[CONFIG_NAME_LEN - 1], '\0');
}

/* --------------------------------------------------------------------- */
/* Unknown tags survive round-trip via emit_raw                          */
/* --------------------------------------------------------------------- */

TEST(ConfigCodecForwardCompat, UnknownRecordSurvivesRewrite)
{
    /* Build a stream containing:
     *   1. a known DI record
     *   2. a synthetic "future" record with an unrecognised domain
     *   3. a known system record
     *
     * Decode it: known records go to known places, the unknown tag is
     * captured (here we'll just remember the raw bytes from the iterator).
     *
     * Re-encode it via tlv_writer_emit + tlv_writer_emit_raw: the unknown
     * record must survive byte-for-byte, in its original position. */

    /* --- Step 1: build the original stream --- */
    uint8_t      orig_buf[128] = {};
    tlv_writer_t w;
    tlv_writer_init(&w, orig_buf, sizeof(orig_buf));

    /* Known DI */
    {
        uint8_t value[CONFIG_CODEC_DI_WIRE_SIZE];
        size_t  size = 0;
        ASSERT_EQ(config_codec_encode_di(
                      &g_di_defaults[0], value, sizeof(value), &size),
                  TLV_OK);
        ASSERT_EQ(
            tlv_writer_emit(&w,
                            config_codec_make_tag(CONFIG_CODEC_DOMAIN_DI, 0u),
                            value,
                            size),
            TLV_OK);
    }

    /* Unknown — domain 0x77, index 3, 5 arbitrary value bytes */
    const uint8_t  unknown_value[] = { 0xCA, 0xFE, 0xBA, 0xBE, 0x42 };
    const uint16_t unknown_tag     = config_codec_make_tag(0x77u, 3u);
    ASSERT_EQ(
        tlv_writer_emit(&w, unknown_tag, unknown_value, sizeof(unknown_value)),
        TLV_OK);

    /* Known system */
    {
        uint8_t value[CONFIG_CODEC_SYSTEM_WIRE_SIZE];
        size_t  size = 0;
        ASSERT_EQ(config_codec_encode_system(
                      &g_system_defaults, value, sizeof(value), &size),
                  TLV_OK);
        ASSERT_EQ(
            tlv_writer_emit(&w,
                            config_codec_make_tag(CONFIG_CODEC_DOMAIN_SYSTEM,
                                                  CONFIG_CODEC_SYSTEM_INDEX),
                            value,
                            size),
            TLV_OK);
    }

    const size_t orig_size = tlv_writer_size(&w);

    /* --- Step 2: iterate, capturing the unknown record as raw bytes --- */
    tlv_iter_t it;
    tlv_iter_init(&it, orig_buf, orig_size);

    std::vector<uint8_t> captured_unknown;
    uint16_t             tag;
    const void *         value;
    size_t               value_len;
    while (tlv_iter_next(&it, &tag, &value, &value_len) == TLV_OK)
    {
        const uint8_t domain = config_codec_tag_domain(tag);
        if (domain == CONFIG_CODEC_DOMAIN_DI
            || domain == CONFIG_CODEC_DOMAIN_SYSTEM)
        {
            /* recognised — would dispatch to decoder; not relevant here */
            continue;
        }
        /* Unknown: capture the full record (header + value) verbatim. */
        const size_t    record_total = 4u /* TLV header */ + value_len;
        const uint8_t * record_start = static_cast<const uint8_t *>(value) - 4u;
        captured_unknown.assign(record_start, record_start + record_total);
    }

    ASSERT_FALSE(captured_unknown.empty());

    /* --- Step 3: re-emit known records + captured unknown via emit_raw --- */
    uint8_t      new_buf[128] = {};
    tlv_writer_t w2;
    tlv_writer_init(&w2, new_buf, sizeof(new_buf));

    {
        uint8_t value_bytes[CONFIG_CODEC_DI_WIRE_SIZE];
        size_t  size = 0;
        ASSERT_EQ(
            config_codec_encode_di(
                &g_di_defaults[0], value_bytes, sizeof(value_bytes), &size),
            TLV_OK);
        ASSERT_EQ(
            tlv_writer_emit(&w2,
                            config_codec_make_tag(CONFIG_CODEC_DOMAIN_DI, 0u),
                            value_bytes,
                            size),
            TLV_OK);
    }

    ASSERT_EQ(tlv_writer_emit_raw(
                  &w2, captured_unknown.data(), captured_unknown.size()),
              TLV_OK);

    {
        uint8_t value_bytes[CONFIG_CODEC_SYSTEM_WIRE_SIZE];
        size_t  size = 0;
        ASSERT_EQ(
            config_codec_encode_system(
                &g_system_defaults, value_bytes, sizeof(value_bytes), &size),
            TLV_OK);
        ASSERT_EQ(
            tlv_writer_emit(&w2,
                            config_codec_make_tag(CONFIG_CODEC_DOMAIN_SYSTEM,
                                                  CONFIG_CODEC_SYSTEM_INDEX),
                            value_bytes,
                            size),
            TLV_OK);
    }

    EXPECT_EQ(tlv_writer_size(&w2), orig_size);
    EXPECT_EQ(std::memcmp(orig_buf, new_buf, orig_size), 0)
        << "Unknown record must survive a round-trip byte-for-byte";
}
