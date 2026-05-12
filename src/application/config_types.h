/*****************************************************************************
 * Module:  config_types
 * Purpose: Data model for the configuration manager. Defines one struct per
 *          IO channel type (DI, DO, TC, AI, AO, PCNT, PWM), the system-wide
 *          configuration struct, and the enums that constrain field values.
 *
 *          Numeric struct fields use fixed-width <stdint.h> types and bool
 *          from <stdbool.h>. Constrained fields are typed enums; ISO C
 *          leaves the underlying integer type implementation-defined, so
 *          static asserts at the bottom of this header pin every enum to
 *          int-sized. That guards the on-flash ABI against options like
 *          -fshort-enums silently shrinking a field.
 *
 *          Every enum exposes a trailing _COUNT sentinel for cheap range
 *          checks at the API boundary.
 *
 *          No I/O. No allocation. No API. Just types.
 *****************************************************************************/

#ifndef APPLICATION_CONFIG_TYPES_H
#define APPLICATION_CONFIG_TYPES_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "application/config_limits.h"

/* ------------------------------------------------------------------------- */
/* Fault states                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Shared across DI / DO / PWM. Semantically: "if something has gone wrong
 * (read failure, fault input, etc.), should we hold the last known good
 * value, force the line low, or force the line high?"
 *
 * Encoded so that FAULT_STATE_HOLD == 0. A record that reads back as all
 * zeros (e.g. a freshly initialised cache before defaults are applied)
 * lands in the safest state.
 *
 * Analogue types (AI / AO / TC) extend this with an explicit fault_value
 * field used when fault_state != HOLD, since "low/high" doesn't map.
 */
typedef enum
{
    FAULT_STATE_HOLD = 0,
    FAULT_STATE_LOW  = 1,
    FAULT_STATE_HIGH = 2,
    FAULT_STATE_COUNT
} fault_state_t;

/* ------------------------------------------------------------------------- */
/* Per-type enums                                                            */
/* ------------------------------------------------------------------------- */

typedef enum
{
    DI_POLARITY_ACTIVE_HIGH = 0,
    DI_POLARITY_ACTIVE_LOW  = 1,
    DI_POLARITY_COUNT
} di_polarity_t;

typedef enum
{
    DO_POLARITY_ACTIVE_HIGH = 0,
    DO_POLARITY_ACTIVE_LOW  = 1,
    DO_POLARITY_COUNT
} do_polarity_t;

/* MAX31856-supported thermocouple types. */
typedef enum
{
    TC_TYPE_K = 0, /* the most common; sensible default */
    TC_TYPE_J = 1,
    TC_TYPE_T = 2,
    TC_TYPE_N = 3,
    TC_TYPE_S = 4,
    TC_TYPE_R = 5,
    TC_TYPE_B = 6,
    TC_TYPE_E = 7,
    TC_TYPE_COUNT
} tc_type_t;

typedef enum
{
    TC_UNIT_CELSIUS    = 0,
    TC_UNIT_FAHRENHEIT = 1,
    TC_UNIT_KELVIN     = 2,
    TC_UNIT_COUNT
} tc_unit_t;

typedef enum
{
    AI_INPUT_MODE_VOLTAGE_0_10V     = 0,
    AI_INPUT_MODE_CURRENT_4_20MA_2W = 1,
    AI_INPUT_MODE_CURRENT_4_20MA_3W = 2,
    AI_INPUT_MODE_COUNT
} ai_input_mode_t;

typedef enum
{
    AO_OUTPUT_MODE_VOLTAGE_0_10V  = 0,
    AO_OUTPUT_MODE_CURRENT_4_20MA = 1,
    AO_OUTPUT_MODE_COUNT
} ao_output_mode_t;

/* "COUNTER" rather than "COUNT" to avoid colliding with the _COUNT
 * sentinel convention used everywhere else in this header. */
typedef enum
{
    PCNT_MODE_COUNTER   = 0,
    PCNT_MODE_FREQUENCY = 1,
    PCNT_MODE_COUNT
} pcnt_mode_t;

typedef enum
{
    PCNT_EDGE_RISING  = 0,
    PCNT_EDGE_FALLING = 1,
    PCNT_EDGE_BOTH    = 2,
    PCNT_EDGE_COUNT
} pcnt_edge_t;

typedef enum
{
    CAN_BITRATE_125K = 0,
    CAN_BITRATE_250K = 1,
    CAN_BITRATE_500K = 2,
    CAN_BITRATE_1M   = 3,
    CAN_BITRATE_COUNT
} can_bitrate_t;

typedef enum
{
    NMT_STARTUP_WAIT      = 0, /* wait for an explicit NMT command */
    NMT_STARTUP_AUTOSTART = 1, /* enter Operational on boot */
    NMT_STARTUP_COUNT
} nmt_startup_t;

/* Top-level addressing of an IO record. Used by the TLV tag.
 * Order is deliberately stable: appending to the bottom preserves prior
 * tag values, so older firmware reading newer configs still finds DI/DO
 * records at the same domain index. */
typedef enum
{
    IO_DOMAIN_DI   = 0,
    IO_DOMAIN_DO   = 1,
    IO_DOMAIN_TC   = 2,
    IO_DOMAIN_AI   = 3,
    IO_DOMAIN_AO   = 4,
    IO_DOMAIN_PCNT = 5,
    IO_DOMAIN_PWM  = 6,
    IO_DOMAIN_COUNT
} io_domain_t;

/* ------------------------------------------------------------------------- */
/* Per-type structs                                                          */
/* ------------------------------------------------------------------------- */

typedef struct
{
    char          name[CONFIG_NAME_LEN]; /* null-terminated, 15 usable chars */
    uint16_t      id;                    /* CANopen-style object identifier */
    uint16_t      debounce_ms;
    di_polarity_t polarity;
    fault_state_t fault_state;
    bool          interrupt_enabled;
} di_config_t;

typedef struct
{
    char          name[CONFIG_NAME_LEN];
    uint16_t      id;
    do_polarity_t polarity;
    fault_state_t fault_state;
} do_config_t;

typedef struct
{
    char          name[CONFIG_NAME_LEN];
    uint16_t      id;
    tc_type_t     tc_type;
    tc_unit_t     unit;
    bool          cjc_enabled;
    uint16_t      filter_ms;
    fault_state_t fault_state;
    int16_t fault_value_c10; /* tenths of degree; only when state != HOLD */
} tc_config_t;

/* AI / AO scaling stored as integer (num, den, offset). Engineering value =
 * (raw * scale_num) / scale_den + offset. Integer math only — no FPU
 * dependency, deterministic across builds. */
typedef struct
{
    char            name[CONFIG_NAME_LEN];
    uint16_t        id;
    ai_input_mode_t input_mode;
    uint16_t        filter_ms;
    int32_t         scale_num;
    int32_t         scale_den; /* must be non-zero */
    int32_t         offset;
    fault_state_t   fault_state;
    int32_t         fault_value; /* used when fault_state != HOLD */
} ai_config_t;

typedef struct
{
    char             name[CONFIG_NAME_LEN];
    uint16_t         id;
    ao_output_mode_t output_mode;
    uint16_t      slew_per_s; /* output units per second; 0 = no slew limit */
    int32_t       scale_num;
    int32_t       scale_den;
    int32_t       offset;
    fault_state_t fault_state;
    int32_t       fault_value;
} ao_config_t;

typedef struct
{
    char        name[CONFIG_NAME_LEN];
    uint16_t    id;
    pcnt_mode_t mode;
    pcnt_edge_t edge;
    uint32_t    limit; /* count rollover (CNT mode) or alarm threshold (FREQ) */
    bool        reset_on_read;
} pcnt_config_t;

typedef struct
{
    char          name[CONFIG_NAME_LEN];
    uint16_t      id;
    uint32_t      period_us;     /* PWM period in microseconds; must be > 0 */
    uint16_t      duty_permille; /* 0..1000, parts per thousand */
    fault_state_t fault_state;
    uint16_t      fault_duty_permille;
} pwm_config_t;

typedef struct
{
    uint8_t       canopen_node_id; /* 1..127 per CANopen */
    can_bitrate_t can_bitrate;
    uint16_t      heartbeat_ms;
    uint16_t      sync_window_us;
    nmt_startup_t nmt_startup;
    /* EMCY producer COB-ID. 0 = use the CANopen predefined-connection-set
     * value, i.e. 0x80 + canopen_node_id, computed at NMT startup. A
     * non-zero value is treated as an operator override and used verbatim.
     * Stored separately from the SYNC COB-ID (always 0x80 by spec). */
    uint16_t producer_emcy_cob_id;
} system_config_t;

/* ------------------------------------------------------------------------- */
/* ABI-stability guards                                                      */
/* ------------------------------------------------------------------------- */
/*
 * Pin every enum used as a struct field to int-sized. ISO C leaves the
 * enum underlying integer type implementation-defined, and toolchain
 * options like -fshort-enums can pack enums into the smallest type that
 * fits. That would silently change struct layout, which is unacceptable
 * for persisted records. Builds fail loudly here instead.
 */
static_assert(sizeof(fault_state_t) == sizeof(int),
              "fault_state_t must be int-sized");
static_assert(sizeof(di_polarity_t) == sizeof(int),
              "di_polarity_t must be int-sized");
static_assert(sizeof(do_polarity_t) == sizeof(int),
              "do_polarity_t must be int-sized");
static_assert(sizeof(tc_type_t) == sizeof(int), "tc_type_t must be int-sized");
static_assert(sizeof(tc_unit_t) == sizeof(int), "tc_unit_t must be int-sized");
static_assert(sizeof(ai_input_mode_t) == sizeof(int),
              "ai_input_mode_t must be int-sized");
static_assert(sizeof(ao_output_mode_t) == sizeof(int),
              "ao_output_mode_t must be int-sized");
static_assert(sizeof(pcnt_mode_t) == sizeof(int),
              "pcnt_mode_t must be int-sized");
static_assert(sizeof(pcnt_edge_t) == sizeof(int),
              "pcnt_edge_t must be int-sized");
static_assert(sizeof(can_bitrate_t) == sizeof(int),
              "can_bitrate_t must be int-sized");
static_assert(sizeof(nmt_startup_t) == sizeof(int),
              "nmt_startup_t must be int-sized");
static_assert(sizeof(io_domain_t) == sizeof(int),
              "io_domain_t must be int-sized");

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CONFIG_TYPES_H */
