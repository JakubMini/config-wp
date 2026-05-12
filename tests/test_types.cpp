/*****************************************************************************
 * Module:  test_types
 * Purpose: Unit tests for the IO data model and factory defaults. Four
 *          categories:
 *
 *            1. Struct sizeof upper bounds — silent growth trips the test
 *               so the EEPROM budget gets re-checked before merging.
 *            2. Enum range sanity — every _COUNT sentinel is positive.
 *            3. Defaults sanity — every entry in every defaults table is
 *               in-range, null-terminated, and free of divide-by-zero
 *               surprises (AI/AO scale_den).
 *            4. Fault-state crosswalk — HOLD == 0 so a zero-initialised
 *               record lands in the safest state.
 *****************************************************************************/

#include <gtest/gtest.h>

extern "C"
{
#include "application/config_defaults.h"
#include "application/config_limits.h"
#include "application/config_types.h"
}

/* --------------------------------------------------------------------- */
/* sizeof checks                                                          */
/* --------------------------------------------------------------------- */

/* Exact host-side sizes (64-bit clang on macOS). Tight on purpose: if a
 * field is added the test fires, the budget gets re-checked, and the
 * number above gets bumped along with the on-flash impact noted in the
 * commit message. Cortex-M padding will differ; re-baseline on target. */
TEST(Types, StructSizesWithinBudget)
{
    EXPECT_LE(sizeof(di_config_t), 32u);
    EXPECT_LE(sizeof(do_config_t), 32u);
    EXPECT_LE(sizeof(tc_config_t), 40u);
    EXPECT_LE(sizeof(ai_config_t), 48u);
    EXPECT_LE(sizeof(ao_config_t), 48u);
    EXPECT_LE(sizeof(pcnt_config_t), 36u);
    EXPECT_LE(sizeof(pwm_config_t), 36u);
    EXPECT_LE(sizeof(system_config_t), 32u);
}

/* --------------------------------------------------------------------- */
/* enum range                                                            */
/* --------------------------------------------------------------------- */

TEST(Types, EnumCountsArePositive)
{
    EXPECT_GT((int)FAULT_STATE_COUNT, 0);
    EXPECT_GT((int)DI_POLARITY_COUNT, 0);
    EXPECT_GT((int)DO_POLARITY_COUNT, 0);
    EXPECT_GT((int)TC_TYPE_COUNT, 0);
    EXPECT_GT((int)TC_UNIT_COUNT, 0);
    EXPECT_GT((int)AI_INPUT_MODE_COUNT, 0);
    EXPECT_GT((int)AO_OUTPUT_MODE_COUNT, 0);
    EXPECT_GT((int)PCNT_MODE_COUNT, 0);
    EXPECT_GT((int)PCNT_EDGE_COUNT, 0);
    EXPECT_GT((int)CAN_BITRATE_COUNT, 0);
    EXPECT_GT((int)NMT_STARTUP_COUNT, 0);
    EXPECT_GT((int)IO_DOMAIN_COUNT, 0);
}

/* HOLD must be the zero value so a freshly-zeroed cache lands in the
 * safest fault behaviour. */
TEST(Types, FaultStateHoldIsZero)
{
    EXPECT_EQ((int)FAULT_STATE_HOLD, 0);
}

TEST(Types, IoDomainMatchesChannelTypeCount)
{
    /* Seven IO types: DI, DO, TC, AI, AO, PCNT, PWM. If this trips, adding
     * a new IO type touched config_types.h but not (yet) the codec,
     * defaults table, or JSON shape that all depend on the same set. */
    EXPECT_EQ((int)IO_DOMAIN_COUNT, 7);
}

/* --------------------------------------------------------------------- */
/* per-type defaults sanity                                              */
/* --------------------------------------------------------------------- */

TEST(Defaults, DiTableIsSane)
{
    for (size_t i = 0; i < CONFIG_NUM_DI; ++i)
    {
        ASSERT_EQ(g_di_defaults[i].name[CONFIG_NAME_LEN - 1], '\0');
        EXPECT_LT((int)g_di_defaults[i].polarity, (int)DI_POLARITY_COUNT);
        EXPECT_LT((int)g_di_defaults[i].fault_state, (int)FAULT_STATE_COUNT);
    }
}

TEST(Defaults, DoTableIsSane)
{
    for (size_t i = 0; i < CONFIG_NUM_DO; ++i)
    {
        ASSERT_EQ(g_do_defaults[i].name[CONFIG_NAME_LEN - 1], '\0');
        EXPECT_LT((int)g_do_defaults[i].polarity, (int)DO_POLARITY_COUNT);
        EXPECT_LT((int)g_do_defaults[i].fault_state, (int)FAULT_STATE_COUNT);
    }
}

TEST(Defaults, TcTableIsSane)
{
    for (size_t i = 0; i < CONFIG_NUM_TC; ++i)
    {
        ASSERT_EQ(g_tc_defaults[i].name[CONFIG_NAME_LEN - 1], '\0');
        EXPECT_LT((int)g_tc_defaults[i].tc_type, (int)TC_TYPE_COUNT);
        EXPECT_LT((int)g_tc_defaults[i].unit, (int)TC_UNIT_COUNT);
        EXPECT_LT((int)g_tc_defaults[i].fault_state, (int)FAULT_STATE_COUNT);
    }
}

TEST(Defaults, AiTableIsSane)
{
    for (size_t i = 0; i < CONFIG_NUM_AI; ++i)
    {
        ASSERT_EQ(g_ai_defaults[i].name[CONFIG_NAME_LEN - 1], '\0');
        EXPECT_LT((int)g_ai_defaults[i].input_mode, (int)AI_INPUT_MODE_COUNT);
        EXPECT_LT((int)g_ai_defaults[i].fault_state, (int)FAULT_STATE_COUNT);
        EXPECT_NE(g_ai_defaults[i].scale_den, 0); /* avoid divide-by-zero */
    }
}

TEST(Defaults, AoTableIsSane)
{
    for (size_t i = 0; i < CONFIG_NUM_AO; ++i)
    {
        ASSERT_EQ(g_ao_defaults[i].name[CONFIG_NAME_LEN - 1], '\0');
        EXPECT_LT((int)g_ao_defaults[i].output_mode, (int)AO_OUTPUT_MODE_COUNT);
        EXPECT_LT((int)g_ao_defaults[i].fault_state, (int)FAULT_STATE_COUNT);
        EXPECT_NE(g_ao_defaults[i].scale_den, 0);
    }
}

TEST(Defaults, PcntTableIsSane)
{
    for (size_t i = 0; i < CONFIG_NUM_PCNT; ++i)
    {
        ASSERT_EQ(g_pcnt_defaults[i].name[CONFIG_NAME_LEN - 1], '\0');
        EXPECT_LT((int)g_pcnt_defaults[i].mode, (int)PCNT_MODE_COUNT);
        EXPECT_LT((int)g_pcnt_defaults[i].edge, (int)PCNT_EDGE_COUNT);
    }
}

TEST(Defaults, PwmTableIsSane)
{
    for (size_t i = 0; i < CONFIG_NUM_PWM; ++i)
    {
        ASSERT_EQ(g_pwm_defaults[i].name[CONFIG_NAME_LEN - 1], '\0');
        EXPECT_LT((int)g_pwm_defaults[i].fault_state, (int)FAULT_STATE_COUNT);
        EXPECT_LE((int)g_pwm_defaults[i].duty_permille, 1000);
        EXPECT_LE((int)g_pwm_defaults[i].fault_duty_permille, 1000);
        EXPECT_GT(g_pwm_defaults[i].period_us, 0u);
    }
}

TEST(Defaults, SystemTableIsSane)
{
    EXPECT_GE(g_system_defaults.canopen_node_id, 1);
    EXPECT_LE(g_system_defaults.canopen_node_id, 127);
    EXPECT_LT((int)g_system_defaults.can_bitrate, (int)CAN_BITRATE_COUNT);
    EXPECT_LT((int)g_system_defaults.nmt_startup, (int)NMT_STARTUP_COUNT);
}
