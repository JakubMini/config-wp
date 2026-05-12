/*****************************************************************************
 * Module:  test_config
 * Purpose: Tests for the manager API. Covers:
 *
 *            - lifecycle: init idempotent, defaults on blank EEPROM
 *            - defaults: get-before-set returns the factory value
 *            - round-trip: set then get returns what was set
 *            - validation: NULL pointer, out-of-range index, bad enum,
 *              bad invariant per type
 *            - persistence: set + save + deinit + storage_init(again) +
 *              init reloads from the slot (simulates a reboot via the
 *              memcpy storage stub, which we deliberately do NOT wipe
 *              between init calls)
 *            - defaults restoration: config_reset_defaults() undoes
 *              arbitrary sets
 *            - forward compat: a blob containing an unknown record
 *              loads correctly; config_save re-emits the unknown
 *              verbatim
 *
 *          Each fixture's SetUp re-initialises the storage stub AND the
 *          manager state. Tests are isolated.
 *****************************************************************************/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

extern "C"
{
#include "application/config.h"
#include "application/config_codec.h"
#include "application/config_defaults.h"
#include "application/config_slot.h"
#include "application/crc32.h"
#include "application/tlv.h"
#include "drivers/storage.h"
}

class ConfigTest : public ::testing::Test
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
 * Lifecycle
 * =================================================================== */

TEST_F(ConfigTest, InitIsIdempotent)
{
    EXPECT_EQ(config_init(), CONFIG_OK);
    EXPECT_EQ(config_init(), CONFIG_OK);
}

TEST_F(ConfigTest, DefaultsLoadedOnBlankStorage)
{
    di_config_t out = {};
    ASSERT_EQ(config_get_di(0, &out), CONFIG_OK);
    EXPECT_EQ(out.debounce_ms, g_di_defaults[0].debounce_ms);
    EXPECT_EQ((int)out.fault_state, (int)g_di_defaults[0].fault_state);
}

TEST_F(ConfigTest, ApiBeforeInitReturnsError)
{
    config_deinit();
    di_config_t out = {};
    EXPECT_EQ(config_get_di(0, &out), CONFIG_ERR_NOT_INITIALISED);
    EXPECT_EQ(config_save(), CONFIG_ERR_NOT_INITIALISED);
    EXPECT_EQ(config_reset_defaults(), CONFIG_ERR_NOT_INITIALISED);
    /* restore for TearDown */
    ASSERT_EQ(config_init(), CONFIG_OK);
}

/* ===================================================================
 * Defaults visible through getters
 * =================================================================== */

TEST_F(ConfigTest, GetSystemReturnsDefaults)
{
    system_config_t s = {};
    ASSERT_EQ(config_get_system(&s), CONFIG_OK);
    EXPECT_EQ(s.canopen_node_id, g_system_defaults.canopen_node_id);
    EXPECT_EQ((int)s.can_bitrate, (int)g_system_defaults.can_bitrate);
    EXPECT_EQ(s.producer_emcy_cob_id, 0u); /* sentinel default */
}

/* ===================================================================
 * Per-type round-trip
 * =================================================================== */

TEST_F(ConfigTest, RoundTripDi)
{
    di_config_t in = g_di_defaults[0];
    std::strncpy(in.name, "front_door", CONFIG_NAME_LEN - 1);
    in.id          = 0x1234;
    in.debounce_ms = 50;
    in.polarity    = DI_POLARITY_ACTIVE_LOW;
    in.fault_state = FAULT_STATE_HIGH;
    ASSERT_EQ(config_set_di(3, &in), CONFIG_OK);

    di_config_t out = {};
    ASSERT_EQ(config_get_di(3, &out), CONFIG_OK);
    EXPECT_STREQ(out.name, "front_door");
    EXPECT_EQ(out.id, in.id);
    EXPECT_EQ(out.debounce_ms, in.debounce_ms);
    EXPECT_EQ((int)out.polarity, (int)in.polarity);
    EXPECT_EQ((int)out.fault_state, (int)in.fault_state);
}

TEST_F(ConfigTest, RoundTripAi)
{
    ai_config_t in = g_ai_defaults[0];
    in.scale_num   = 1000;
    in.scale_den   = 4096;
    in.offset      = -50;
    in.fault_value = 9999;
    ASSERT_EQ(config_set_ai(2, &in), CONFIG_OK);

    ai_config_t out = {};
    ASSERT_EQ(config_get_ai(2, &out), CONFIG_OK);
    EXPECT_EQ(out.scale_num, in.scale_num);
    EXPECT_EQ(out.scale_den, in.scale_den);
    EXPECT_EQ(out.offset, in.offset);
    EXPECT_EQ(out.fault_value, in.fault_value);
}

TEST_F(ConfigTest, RoundTripSystem)
{
    system_config_t in      = g_system_defaults;
    in.canopen_node_id      = 42;
    in.can_bitrate          = CAN_BITRATE_1M;
    in.heartbeat_ms         = 250;
    in.producer_emcy_cob_id = 0x82; /* valid override */
    ASSERT_EQ(config_set_system(&in), CONFIG_OK);

    system_config_t out = {};
    ASSERT_EQ(config_get_system(&out), CONFIG_OK);
    EXPECT_EQ(out.canopen_node_id, 42);
    EXPECT_EQ((int)out.can_bitrate, (int)CAN_BITRATE_1M);
    EXPECT_EQ(out.producer_emcy_cob_id, 0x82);
}

/* ===================================================================
 * Validation
 * =================================================================== */

TEST_F(ConfigTest, NullPointerRejected)
{
    EXPECT_EQ(config_get_di(0, NULL), CONFIG_ERR_INVALID);
    EXPECT_EQ(config_set_di(0, NULL), CONFIG_ERR_INVALID);
    EXPECT_EQ(config_get_system(NULL), CONFIG_ERR_INVALID);
    EXPECT_EQ(config_set_system(NULL), CONFIG_ERR_INVALID);
}

TEST_F(ConfigTest, OutOfRangeIndexRejected)
{
    di_config_t in = g_di_defaults[0];
    EXPECT_EQ(config_set_di(CONFIG_NUM_DI, &in), CONFIG_ERR_INDEX);
    EXPECT_EQ(config_set_di(255, &in), CONFIG_ERR_INDEX);
    di_config_t out = {};
    EXPECT_EQ(config_get_di(CONFIG_NUM_DI, &out), CONFIG_ERR_INDEX);
}

TEST_F(ConfigTest, BadEnumValueRejected)
{
    di_config_t in = g_di_defaults[0];
    in.polarity    = (di_polarity_t)99;
    EXPECT_EQ(config_set_di(0, &in), CONFIG_ERR_INVALID);
}

TEST_F(ConfigTest, AiScaleDenZeroRejected)
{
    ai_config_t in = g_ai_defaults[0];
    in.scale_den   = 0;
    EXPECT_EQ(config_set_ai(0, &in), CONFIG_ERR_INVALID);
}

TEST_F(ConfigTest, PwmInvalidDutyRejected)
{
    pwm_config_t in  = g_pwm_defaults[0];
    in.duty_permille = 1001;
    EXPECT_EQ(config_set_pwm(0, &in), CONFIG_ERR_INVALID);
}

TEST_F(ConfigTest, PwmZeroPeriodRejected)
{
    pwm_config_t in = g_pwm_defaults[0];
    in.period_us    = 0;
    EXPECT_EQ(config_set_pwm(0, &in), CONFIG_ERR_INVALID);
}

TEST_F(ConfigTest, SystemNodeIdRangeEnforced)
{
    system_config_t in = g_system_defaults;
    in.canopen_node_id = 0;
    EXPECT_EQ(config_set_system(&in), CONFIG_ERR_INVALID);
    in.canopen_node_id = 128;
    EXPECT_EQ(config_set_system(&in), CONFIG_ERR_INVALID);
    in.canopen_node_id = 127;
    EXPECT_EQ(config_set_system(&in), CONFIG_OK);
}

TEST_F(ConfigTest, SystemEmcyCobIdSentinelOrOverride)
{
    system_config_t in      = g_system_defaults;
    in.producer_emcy_cob_id = 0; /* sentinel */
    EXPECT_EQ(config_set_system(&in), CONFIG_OK);
    in.producer_emcy_cob_id = 0x80; /* clashes with SYNC */
    EXPECT_EQ(config_set_system(&in), CONFIG_ERR_INVALID);
    in.producer_emcy_cob_id = 0x81; /* valid override */
    EXPECT_EQ(config_set_system(&in), CONFIG_OK);
}

TEST_F(ConfigTest, NameNotNullTerminatedRejected)
{
    di_config_t in = g_di_defaults[0];
    std::memset(in.name, 'A', sizeof(in.name)); /* no NUL anywhere */
    EXPECT_EQ(config_set_di(0, &in), CONFIG_ERR_INVALID);
}

/* ===================================================================
 * Defaults restoration
 * =================================================================== */

TEST_F(ConfigTest, ResetDefaultsRestoresAllFields)
{
    /* Mutate a few records. */
    di_config_t di = g_di_defaults[0];
    di.debounce_ms = 999;
    ASSERT_EQ(config_set_di(0, &di), CONFIG_OK);

    system_config_t sys = g_system_defaults;
    sys.canopen_node_id = 50;
    ASSERT_EQ(config_set_system(&sys), CONFIG_OK);

    ASSERT_EQ(config_reset_defaults(), CONFIG_OK);

    di_config_t di_out = {};
    ASSERT_EQ(config_get_di(0, &di_out), CONFIG_OK);
    EXPECT_EQ(di_out.debounce_ms, g_di_defaults[0].debounce_ms);

    system_config_t sys_out = {};
    ASSERT_EQ(config_get_system(&sys_out), CONFIG_OK);
    EXPECT_EQ(sys_out.canopen_node_id, g_system_defaults.canopen_node_id);
}

/* ===================================================================
 * Persistence — save, deinit, re-init, get
 * =================================================================== */

TEST_F(ConfigTest, SaveAndReloadPersistsAcrossInit)
{
    di_config_t in = g_di_defaults[0];
    std::strncpy(in.name, "saved", CONFIG_NAME_LEN - 1);
    in.debounce_ms = 77;
    ASSERT_EQ(config_set_di(5, &in), CONFIG_OK);

    system_config_t sys      = g_system_defaults;
    sys.canopen_node_id      = 33;
    sys.producer_emcy_cob_id = 0xA0;
    ASSERT_EQ(config_set_system(&sys), CONFIG_OK);

    ASSERT_EQ(config_save(), CONFIG_OK);

    /* Simulate a reboot: tear down the manager but DO NOT call
     * storage_init — that would wipe the memcpy stub's bytes and lose
     * the slot we just wrote. Re-initing only the manager mirrors what
     * a real power cycle looks like (storage persists, manager state
     * doesn't). */
    config_deinit();
    ASSERT_EQ(config_init(), CONFIG_OK);

    di_config_t out = {};
    ASSERT_EQ(config_get_di(5, &out), CONFIG_OK);
    EXPECT_STREQ(out.name, "saved");
    EXPECT_EQ(out.debounce_ms, 77u);

    system_config_t sys_out = {};
    ASSERT_EQ(config_get_system(&sys_out), CONFIG_OK);
    EXPECT_EQ(sys_out.canopen_node_id, 33);
    EXPECT_EQ(sys_out.producer_emcy_cob_id, 0xA0);
}

TEST_F(ConfigTest, SaveBlankStorageRoundTripsCleanly)
{
    /* Just save defaults and reload — make sure defaults survive a round
     * trip through the codec / slot / decoder. */
    ASSERT_EQ(config_save(), CONFIG_OK);

    config_deinit();
    ASSERT_EQ(config_init(), CONFIG_OK);

    di_config_t di = {};
    ASSERT_EQ(config_get_di(0, &di), CONFIG_OK);
    EXPECT_EQ(di.debounce_ms, g_di_defaults[0].debounce_ms);
    EXPECT_EQ((int)di.fault_state, (int)g_di_defaults[0].fault_state);
}

/* ===================================================================
 * Forward compatibility — unknown record survives manager save/load
 * =================================================================== */

TEST_F(ConfigTest, UnknownRecordSurvivesManagerSaveLoad)
{
    /* Save a clean blob first so we have known starting state. */
    ASSERT_EQ(config_save(), CONFIG_OK);

    /* Now manually craft a blob that includes one known DI record and
     * one unknown record from a fictional future domain, and slot_write
     * it. config_init() will load it; config_save() must re-emit the
     * unknown verbatim. */
    uint8_t      blob[256] = {};
    tlv_writer_t w;
    tlv_writer_init(&w, blob, sizeof(blob));

    /* known DI 0 with a tweaked debounce */
    di_config_t di     = g_di_defaults[0];
    di.debounce_ms     = 123;
    uint8_t value[64]  = {};
    size_t  value_size = 0;
    ASSERT_EQ(config_codec_encode_di(&di, value, sizeof(value), &value_size),
              TLV_OK);
    ASSERT_EQ(tlv_writer_emit(&w,
                              config_codec_make_tag(CONFIG_CODEC_DOMAIN_DI, 0),
                              value,
                              value_size),
              TLV_OK);

    /* unknown — domain 0x77, index 9, 5 arbitrary bytes */
    const uint8_t  unknown_value[] = { 0xCA, 0xFE, 0xBA, 0xBE, 0x42 };
    const uint16_t unknown_tag     = config_codec_make_tag(0x77u, 9u);
    ASSERT_EQ(
        tlv_writer_emit(&w, unknown_tag, unknown_value, sizeof(unknown_value)),
        TLV_OK);

    const size_t blob_len = tlv_writer_size(&w);

    /* Inject the crafted blob via the slot layer. */
    config_deinit();
    ASSERT_EQ(storage_init(), STORAGE_OK); /* wipe slots */
    crc32_init();
    ASSERT_EQ(slot_write(blob, blob_len), SLOT_OK);

    /* Load via the manager; the DI tweak must come through, the
     * unknown must be preserved internally. */
    ASSERT_EQ(config_init(), CONFIG_OK);
    di_config_t di_out = {};
    ASSERT_EQ(config_get_di(0, &di_out), CONFIG_OK);
    EXPECT_EQ(di_out.debounce_ms, 123u);

    /* Save again and verify the new slot blob STILL contains the
     * unknown record byte-for-byte. */
    ASSERT_EQ(config_save(), CONFIG_OK);

    uint8_t   reloaded[SLOT_PAYLOAD_MAX_BYTES] = {};
    slot_id_t id                               = SLOT_NONE;
    size_t    len                              = 0;
    ASSERT_EQ(slot_pick_active(&id, reloaded, sizeof(reloaded), &len), SLOT_OK);

    /* Walk the reloaded blob, find the unknown tag. */
    tlv_iter_t it;
    tlv_iter_init(&it, reloaded, len);
    bool found_unknown = false;
    for (;;)
    {
        uint16_t     tag = 0;
        const void * v   = NULL;
        size_t       vl  = 0;
        tlv_status_t s   = tlv_iter_next(&it, &tag, &v, &vl);
        if (s == TLV_END)
        {
            break;
        }
        ASSERT_EQ(s, TLV_OK);
        if (tag == unknown_tag)
        {
            ASSERT_EQ(vl, sizeof(unknown_value));
            EXPECT_EQ(std::memcmp(v, unknown_value, sizeof(unknown_value)), 0);
            found_unknown = true;
        }
    }
    EXPECT_TRUE(found_unknown)
        << "unknown record must survive manager save/load round-trip";
}
