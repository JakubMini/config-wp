/*****************************************************************************
 * Module:  test_config_json
 * Purpose: Tests for the JSON wrapper.
 *
 *          Categories:
 *            1. Export shape — well-formed JSON, contains expected
 *               top-level keys + per-record "channel" fields.
 *            2. Round-trip — set values → export → reset → import →
 *               get values returns the original set values.
 *            3. Single source of validation — JSON with a bad enum
 *               or out-of-range field gets rejected by the same Phase 4
 *               setter the C API uses.
 *            4. Malformed JSON — parse errors return CONFIG_ERR_CODEC.
 *            5. Addressing contract — records must carry an explicit
 *               in-range non-duplicate `channel`. Missing / non-integer
 *               / out-of-range / duplicate channels all reject.
 *            6. Partial update — one-element array with explicit channel
 *               touches exactly that channel; missing fields preserved.
 *            7. Forward compat — unknown top-level keys tolerated and
 *               counted; unknown fields within a record tolerated.
 *            8. Report semantics — first_error describes the first
 *               failure.
 *****************************************************************************/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

extern "C"
{
#include "application/config.h"
#include "application/config_defaults.h"
#include "application/config_json.h"
#include "application/config_limits.h"
#include "drivers/storage.h"
}

class JsonTest : public ::testing::Test
{
protected:
    void SetUp () override
    {
        config_deinit();
        ASSERT_EQ(storage_init(), STORAGE_OK);
        ASSERT_EQ(config_init(), CONFIG_OK);
    }
    void TearDown () override
    {
        config_deinit();
    }
};

/* ===================================================================
 * Export shape
 * =================================================================== */

TEST_F(JsonTest, ExportProducesValidJson)
{
    std::vector<char> buf(16 * 1024, 0);
    size_t            written = 0;
    ASSERT_EQ(config_export_json(buf.data(), buf.size(), &written), CONFIG_OK);
    EXPECT_GT(written, 100u);
    EXPECT_LT(written, buf.size());
    EXPECT_EQ(written, std::strlen(buf.data()));
    /* Headline keys present. */
    EXPECT_NE(std::strstr(buf.data(), "\"format\""), nullptr);
    EXPECT_NE(std::strstr(buf.data(), "\"system\""), nullptr);
    EXPECT_NE(std::strstr(buf.data(), "\"di\""), nullptr);
    EXPECT_NE(std::strstr(buf.data(), "\"pwm\""), nullptr);
    /* Per-record "channel" addressing emitted. */
    EXPECT_NE(std::strstr(buf.data(), "\"channel\""), nullptr);
    /* Enum strings rendered, not raw integers. */
    EXPECT_NE(std::strstr(buf.data(), "ACTIVE_HIGH"), nullptr);
    EXPECT_NE(std::strstr(buf.data(), "500K"), nullptr);
    EXPECT_NE(std::strstr(buf.data(), "HOLD"), nullptr);
}

TEST_F(JsonTest, ExportTooSmallBufferRejected)
{
    char   tiny[16] = {};
    size_t written  = 0;
    EXPECT_EQ(config_export_json(tiny, sizeof(tiny), &written),
              CONFIG_ERR_TOO_LARGE);
}

TEST_F(JsonTest, ExportNullBufferRejected)
{
    size_t written = 0;
    EXPECT_EQ(config_export_json(nullptr, 1024, &written), CONFIG_ERR_INVALID);
    char buf[64] = {};
    EXPECT_EQ(config_export_json(buf, sizeof(buf), nullptr),
              CONFIG_ERR_INVALID);
}

/* ===================================================================
 * Round-trip
 * =================================================================== */

TEST_F(JsonTest, RoundTripModifiedValues)
{
    /* Mutate a DI and the system block. */
    di_config_t di = g_di_defaults[3];
    std::strncpy(di.name, "front_door", CONFIG_NAME_LEN - 1);
    di.id                = 0x1234;
    di.debounce_ms       = 42;
    di.polarity          = DI_POLARITY_ACTIVE_LOW;
    di.fault_state       = FAULT_STATE_HIGH;
    di.interrupt_enabled = true;
    ASSERT_EQ(config_set_di(3, &di), CONFIG_OK);

    system_config_t sys      = g_system_defaults;
    sys.canopen_node_id      = 42;
    sys.can_bitrate          = CAN_BITRATE_1M;
    sys.heartbeat_ms         = 250;
    sys.producer_emcy_cob_id = 0x82;
    ASSERT_EQ(config_set_system(&sys), CONFIG_OK);

    /* Export. */
    std::vector<char> buf(16 * 1024, 0);
    size_t            written = 0;
    ASSERT_EQ(config_export_json(buf.data(), buf.size(), &written), CONFIG_OK);

    /* Reset to defaults so we can verify the import restored the mutations. */
    ASSERT_EQ(config_reset_defaults(), CONFIG_OK);

    config_import_report_t report;
    ASSERT_EQ(config_import_json(buf.data(), written, &report), CONFIG_OK);
    EXPECT_EQ(report.rejected, 0u);
    EXPECT_EQ(report.malformed, 0u);
    EXPECT_GT(report.accepted, 0u);

    /* Verify the mutations came back through the JSON. */
    di_config_t di_out = {};
    ASSERT_EQ(config_get_di(3, &di_out), CONFIG_OK);
    EXPECT_STREQ(di_out.name, "front_door");
    EXPECT_EQ(di_out.id, 0x1234);
    EXPECT_EQ(di_out.debounce_ms, 42);
    EXPECT_EQ((int)di_out.polarity, (int)DI_POLARITY_ACTIVE_LOW);
    EXPECT_EQ((int)di_out.fault_state, (int)FAULT_STATE_HIGH);
    EXPECT_TRUE(di_out.interrupt_enabled);

    system_config_t sys_out = {};
    ASSERT_EQ(config_get_system(&sys_out), CONFIG_OK);
    EXPECT_EQ(sys_out.canopen_node_id, 42);
    EXPECT_EQ((int)sys_out.can_bitrate, (int)CAN_BITRATE_1M);
    EXPECT_EQ(sys_out.heartbeat_ms, 250);
    EXPECT_EQ(sys_out.producer_emcy_cob_id, 0x82u);
}

/* ===================================================================
 * Single source of validation: bad inputs hit the Phase-4 setter
 * =================================================================== */

TEST_F(JsonTest, BadEnumStringRejected)
{
    const char * json
        = "{\"di\": [ {\"channel\": 0, \"polarity\": \"INSIDE_OUT\"} ]}";
    config_import_report_t r;
    EXPECT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_GT(r.rejected, 0u);
    EXPECT_NE(r.first_error[0], '\0');
}

TEST_F(JsonTest, SetterValidationStillEnforced)
{
    /* The setter rejects PWM duty > 1000. Drive that path via JSON. */
    const char * json
        = "{\"pwm\": [ {\"channel\": 0, "
          "\"period_us\": 1000, \"duty_permille\": 9999} ]}";
    config_import_report_t r;
    EXPECT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_GT(r.rejected, 0u);
    EXPECT_NE(std::strstr(r.first_error, "pwm"), nullptr);
}

TEST_F(JsonTest, SetterValidationOnSystemEmcyOverride)
{
    /* 0x80 collides with SYNC; the system validator rejects it. */
    const char * json
        = "{\"system\": {\"producer_emcy_cob_id\": 128}}"; /* 128 == 0x80 */
    config_import_report_t r;
    EXPECT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_GT(r.rejected, 0u);
}

/* ===================================================================
 * Malformed JSON
 * =================================================================== */

TEST_F(JsonTest, MalformedJsonReturnsCodecErr)
{
    const char * bad = "{not a json}";
    EXPECT_EQ(config_import_json(bad, std::strlen(bad), nullptr),
              CONFIG_ERR_CODEC);
}

TEST_F(JsonTest, NonObjectRootReturnsInvalid)
{
    const char * bad = "[1, 2, 3]";
    EXPECT_EQ(config_import_json(bad, std::strlen(bad), nullptr),
              CONFIG_ERR_INVALID);
}

TEST_F(JsonTest, NullJsonReturnsInvalid)
{
    EXPECT_EQ(config_import_json(nullptr, 0, nullptr), CONFIG_ERR_INVALID);
}

/* ===================================================================
 * Addressing contract — channel must be present, integer, in range,
 * unique within the array.
 * =================================================================== */

TEST_F(JsonTest, MissingChannelFieldRejected)
{
    const char *           json = "{\"di\": [ {\"debounce_ms\": 30} ]}";
    config_import_report_t r;
    EXPECT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.rejected, 1u);
    EXPECT_NE(std::strstr(r.first_error, "channel"), nullptr)
        << "first_error must explain missing channel, got: " << r.first_error;
}

TEST_F(JsonTest, NonIntegerChannelRejected)
{
    const char * json
        = "{\"di\": [ {\"channel\": \"front_door\", \"debounce_ms\": 30} ]}";
    config_import_report_t r;
    EXPECT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.rejected, 1u);
    EXPECT_NE(std::strstr(r.first_error, "channel"), nullptr);
}

TEST_F(JsonTest, ChannelOutOfRangeRejected)
{
    /* CONFIG_NUM_DI is 16; channel 99 is out of range. */
    const char * json = "{\"di\": [ {\"channel\": 99, \"debounce_ms\": 30} ]}";
    config_import_report_t r;
    EXPECT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 0u);
    EXPECT_EQ(r.rejected, 1u);
    EXPECT_NE(std::strstr(r.first_error, "out of range"), nullptr);
}

TEST_F(JsonTest, DuplicateChannelRejectedAfterFirst)
{
    /* Two records both claim channel 3. First applies, second rejected. */
    const char * json
        = "{\"di\": ["
          "{\"channel\": 3, \"debounce_ms\": 11},"
          "{\"channel\": 3, \"debounce_ms\": 22}"
          "]}";
    config_import_report_t r;
    ASSERT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 1u);
    EXPECT_EQ(r.rejected, 1u);
    EXPECT_NE(std::strstr(r.first_error, "duplicate"), nullptr);

    /* First wins: di[3].debounce_ms is 11, not 22. */
    di_config_t got = {};
    ASSERT_EQ(config_get_di(3, &got), CONFIG_OK);
    EXPECT_EQ(got.debounce_ms, 11);
}

TEST_F(JsonTest, NullArrayEntryIsSilentNoOp)
{
    /* null entries carry no addressing info — accept silently, neither
     * accept nor reject. The real record at channel 2 still applies. */
    const char * json
        = "{\"di\": [null, null, {\"channel\": 2, \"debounce_ms\": 77}, "
          "null]}";
    config_import_report_t r;
    ASSERT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 1u);
    EXPECT_EQ(r.rejected, 0u);
    EXPECT_EQ(r.malformed, 0u);

    di_config_t got = {};
    ASSERT_EQ(config_get_di(2, &got), CONFIG_OK);
    EXPECT_EQ(got.debounce_ms, 77);
}

/* ===================================================================
 * Partial update
 * =================================================================== */

TEST_F(JsonTest, PartialUpdateLeavesOtherChannelsAlone)
{
    /* Dirty two DIs first. */
    di_config_t di_a = g_di_defaults[0];
    di_a.debounce_ms = 11;
    di_a.id          = 0x00AA;
    ASSERT_EQ(config_set_di(0, &di_a), CONFIG_OK);

    di_config_t di_b = g_di_defaults[5];
    di_b.debounce_ms = 22;
    di_b.id          = 0x00BB;
    ASSERT_EQ(config_set_di(5, &di_b), CONFIG_OK);

    /* One-element JSON addresses channel 5 only. No leading nulls
     * needed — order is irrelevant under explicit addressing. */
    const char * json = "{\"di\": [{\"channel\": 5, \"debounce_ms\": 99}]}";

    config_import_report_t r;
    ASSERT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.rejected, 0u);
    EXPECT_EQ(r.accepted, 1u);

    /* di[0] unchanged. */
    di_config_t got0 = {};
    ASSERT_EQ(config_get_di(0, &got0), CONFIG_OK);
    EXPECT_EQ(got0.debounce_ms, 11);
    EXPECT_EQ(got0.id, 0x00AA);

    /* di[5]: debounce_ms updated, id preserved from cache. */
    di_config_t got5 = {};
    ASSERT_EQ(config_get_di(5, &got5), CONFIG_OK);
    EXPECT_EQ(got5.debounce_ms, 99);
    EXPECT_EQ(got5.id, 0x00BB);
}

TEST_F(JsonTest, MissingFieldKeepsCachedValue)
{
    /* Set a non-default name on di[2]. */
    di_config_t in = g_di_defaults[2];
    std::strncpy(in.name, "keep-me", CONFIG_NAME_LEN - 1);
    in.id          = 0xBEEF;
    in.debounce_ms = 77;
    ASSERT_EQ(config_set_di(2, &in), CONFIG_OK);

    /* JSON updates id only; name and debounce_ms must survive. */
    const char *           json = "{\"di\": [{\"channel\": 2, \"id\": 5}]}";
    config_import_report_t r;
    ASSERT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);

    di_config_t got = {};
    ASSERT_EQ(config_get_di(2, &got), CONFIG_OK);
    EXPECT_EQ(got.id, 5);
    EXPECT_STREQ(got.name, "keep-me");
    EXPECT_EQ(got.debounce_ms, 77);
}

TEST_F(JsonTest, OutOfOrderChannelsApplyCorrectly)
{
    /* Records can appear in any order. Apply channel 7 then channel 2;
     * both end up at the right cache slot. */
    const char * json
        = "{\"di\": ["
          "{\"channel\": 7, \"debounce_ms\": 70},"
          "{\"channel\": 2, \"debounce_ms\": 20}"
          "]}";
    config_import_report_t r;
    ASSERT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 2u);
    EXPECT_EQ(r.rejected, 0u);

    di_config_t got2 = {};
    di_config_t got7 = {};
    ASSERT_EQ(config_get_di(2, &got2), CONFIG_OK);
    ASSERT_EQ(config_get_di(7, &got7), CONFIG_OK);
    EXPECT_EQ(got2.debounce_ms, 20);
    EXPECT_EQ(got7.debounce_ms, 70);
}

/* ===================================================================
 * Forward compat
 * =================================================================== */

TEST_F(JsonTest, UnknownTopLevelKeyTolerated)
{
    const char * json
        = "{\"format\": 2, \"future_feature\": {\"foo\": 1}, "
          "\"di\": []}";
    config_import_report_t r;
    ASSERT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.rejected, 0u);
    EXPECT_EQ(r.unknown_keys, 1u);
}

TEST_F(JsonTest, UnknownFieldInsideRecordIgnored)
{
    /* cJSON's get-object-by-key returns NULL for unknown keys; our
     * patch functions only update on present keys, so unknowns are
     * silently ignored within a record. The setter still validates
     * everything else. */
    const char * json
        = "{\"di\": [{\"channel\": 0, "
          "\"debounce_ms\": 30, \"new_field_2027\": 42}]}";
    config_import_report_t r;
    ASSERT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 1u);
    EXPECT_EQ(r.rejected, 0u);

    di_config_t got = {};
    ASSERT_EQ(config_get_di(0, &got), CONFIG_OK);
    EXPECT_EQ(got.debounce_ms, 30);
}

TEST_F(JsonTest, IntegerEnumAcceptedAsAlternative)
{
    /* Tool friendliness: enums can also be sent as numbers. */
    const char * json
        = "{\"di\": [{\"channel\": 0, \"polarity\": 1}]}"; /* 1 == ACTIVE_LOW */
    config_import_report_t r;
    ASSERT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.accepted, 1u);
    di_config_t got = {};
    ASSERT_EQ(config_get_di(0, &got), CONFIG_OK);
    EXPECT_EQ((int)got.polarity, (int)DI_POLARITY_ACTIVE_LOW);
}

/* ===================================================================
 * Report semantics
 * =================================================================== */

TEST_F(JsonTest, ReportFirstErrorIsFirstFailure)
{
    /* Multiple bad records: first_error should describe the FIRST
     * failure (channel 0's bad enum), not a later one. */
    const char * json
        = "{\"di\": ["
          "{\"channel\": 0, \"polarity\": \"NOPE\"}, "
          "{\"channel\": 1, \"polarity\": \"ALSO_BAD\"}"
          "]}";
    config_import_report_t r;
    ASSERT_EQ(config_import_json(json, std::strlen(json), &r), CONFIG_OK);
    EXPECT_EQ(r.rejected, 2u);
    EXPECT_NE(std::strstr(r.first_error, "ch=0"), nullptr)
        << "first_error must describe the FIRST failure, got: "
        << r.first_error;
}
