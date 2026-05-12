/*****************************************************************************
 * Module:  config_defaults
 * Purpose: Static const factory-default tables for every IO type and the
 *          system configuration. See config_defaults.h for the contract.
 *
 *          Defaults use the gcc/clang range-initialiser extension
 *
 *              [0 ... N - 1] = { ... }
 *
 *          to set every channel to the same baseline without per-index
 *          repetition. This is a deliberate non-ISO deviation, supported by
 *          both the host toolchain (apple clang) and the eventual target
 *          toolchain (arm-none-eabi-gcc). It's worth the deviation because
 *          per-channel defaults are identical, and per-index initialisers
 *          for 16-channel arrays are an error-prone tax for zero benefit.
 *****************************************************************************/

#include "application/config_defaults.h"

/* The GNU array-range designator [0 ... N-1] = { ... } is rejected by
 * -Wpedantic. Suppress *locally* — the file is otherwise pure data so the
 * suppression doesn't mask any other potential warnings. clang and gcc both
 * understand this pragma form. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-designator"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/* clang-format off */
/* (range-init blocks below want a stable per-field layout that clang-format
 * occasionally reflows into a less readable shape) */

const di_config_t g_di_defaults[CONFIG_NUM_DI] = {
    [0 ... CONFIG_NUM_DI - 1] = {
        .name              = "",
        .id                = 0,
        .debounce_ms       = 10,
        .polarity          = DI_POLARITY_ACTIVE_HIGH,
        .fault_state       = FAULT_STATE_HOLD,
        .interrupt_enabled = false,
    },
};

const do_config_t g_do_defaults[CONFIG_NUM_DO] = {
    [0 ... CONFIG_NUM_DO - 1] = {
        .name        = "",
        .id          = 0,
        .polarity    = DO_POLARITY_ACTIVE_HIGH,
        .fault_state = FAULT_STATE_LOW,
    },
};

const tc_config_t g_tc_defaults[CONFIG_NUM_TC] = {
    [0 ... CONFIG_NUM_TC - 1] = {
        .name            = "",
        .id              = 0,
        .tc_type         = TC_TYPE_K,
        .unit            = TC_UNIT_CELSIUS,
        .cjc_enabled     = true,
        .filter_ms       = 100,
        .fault_state     = FAULT_STATE_HOLD,
        .fault_value_c10 = 0,
    },
};

const ai_config_t g_ai_defaults[CONFIG_NUM_AI] = {
    [0 ... CONFIG_NUM_AI - 1] = {
        .name        = "",
        .id          = 0,
        .input_mode  = AI_INPUT_MODE_VOLTAGE_0_10V,
        .filter_ms   = 10,
        .scale_num   = 1,
        .scale_den   = 1,
        .offset      = 0,
        .fault_state = FAULT_STATE_HOLD,
        .fault_value = 0,
    },
};

const ao_config_t g_ao_defaults[CONFIG_NUM_AO] = {
    [0 ... CONFIG_NUM_AO - 1] = {
        .name        = "",
        .id          = 0,
        .output_mode = AO_OUTPUT_MODE_VOLTAGE_0_10V,
        .slew_per_s  = 0,
        .scale_num   = 1,
        .scale_den   = 1,
        .offset      = 0,
        .fault_state = FAULT_STATE_LOW,
        .fault_value = 0,
    },
};

const pcnt_config_t g_pcnt_defaults[CONFIG_NUM_PCNT] = {
    [0 ... CONFIG_NUM_PCNT - 1] = {
        .name          = "",
        .id            = 0,
        .mode          = PCNT_MODE_COUNTER,
        .edge          = PCNT_EDGE_RISING,
        .limit         = 0,
        .reset_on_read = false,
    },
};

const pwm_config_t g_pwm_defaults[CONFIG_NUM_PWM] = {
    [0 ... CONFIG_NUM_PWM - 1] = {
        .name                = "",
        .id                  = 0,
        .period_us           = 10000, /* 100 Hz */
        .duty_permille       = 0,
        .fault_state         = FAULT_STATE_LOW,
        .fault_duty_permille = 0,
    },
};

const system_config_t g_system_defaults = {
    .canopen_node_id      = 1,
    .can_bitrate          = CAN_BITRATE_500K,
    .heartbeat_ms         = 1000,
    .sync_window_us       = 1000,
    .nmt_startup          = NMT_STARTUP_WAIT,
    .producer_emcy_cob_id = 0x80,
};

/* clang-format on */

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
