/*****************************************************************************
 * Module:  eeprom_manager
 * Purpose: FreeRTOS task that owns persistence of the configuration
 *          cache. Other tasks queue change requests through this API;
 *          the manager applies them to the in-RAM cache (via the
 *          config manager's validating setters) and writes the result
 *          to storage. Concentrating storage I/O in one task keeps
 *          config_save() single-writer (matches its contract) and
 *          gives natural coalescing: a burst of piecewise updates
 *          becomes one slot write.
 *
 *          Lifecycle:
 *            - eeprom_manager_init() runs synchronously, before the
 *              scheduler, and loads the persisted config (or factory
 *              defaults if storage is blank/corrupt) via config_init().
 *              "Runs first on boot to load initial settings."
 *            - The task itself idles on its queue and drains it as
 *              requests arrive.
 *
 *          Two flavours of request:
 *            1. Piecewise field updates — QueueConfigChange(...).
 *               Use when one parameter changes from an isolated source
 *               (operator CLI, CAN SDO, etc.). The manager applies the
 *               field via config_set_<type>() and re-saves.
 *            2. Monolithic commit — QueueConfigCommit().
 *               Use when a caller has already mutated the cache
 *               directly (config_import_json on a JSON blob) and only
 *               needs a save trigger. The manager just calls
 *               config_save().
 *
 *          Threading:
 *            - QueueConfigChange / QueueConfigCommit are safe to call
 *              from any task. They are non-blocking (short timeout on
 *              xQueueSend) and return a bool indicating whether the
 *              request was enqueued.
 *            - The manager task is the ONLY caller of config_save().
 *
 *          Coalescing: the task tries to drain the entire queue before
 *          calling config_save(), so a burst of 10 piecewise updates
 *          produces one EEPROM write — good for flash/EEPROM endurance.
 *****************************************************************************/

#ifndef APPLICATION_EEPROM_MANAGER_H
#define APPLICATION_EEPROM_MANAGER_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "application/config.h"

/* ----- Request taxonomy -------------------------------------------- */

/* Top-level addressing — selects which struct in the cache is being
 * targeted. The IO_* values intentionally mirror io_domain_t so that
 * patterns from elsewhere in the codebase (TLV tags, JSON dispatch)
 * carry over directly. */
typedef enum
{
    EEPROM_TYPE_IO_DI   = 0,
    EEPROM_TYPE_IO_DO   = 1,
    EEPROM_TYPE_IO_TC   = 2,
    EEPROM_TYPE_IO_AI   = 3,
    EEPROM_TYPE_IO_AO   = 4,
    EEPROM_TYPE_IO_PCNT = 5,
    EEPROM_TYPE_IO_PWM  = 6,
    EEPROM_TYPE_SYSTEM  = 7,
    EEPROM_TYPE_COUNT
} eeprom_config_type_t;

/* Per-domain field IDs. The dispatcher casts `param` to the right enum
 * once it knows `type`. */

typedef enum
{
    DI_PARAM_NAME = 0,
    DI_PARAM_ID,
    DI_PARAM_DEBOUNCE_MS,
    DI_PARAM_POLARITY,
    DI_PARAM_FAULT_STATE,
    DI_PARAM_INTERRUPT_ENABLED,
    DI_PARAM_COUNT
} di_param_t;

typedef enum
{
    DO_PARAM_NAME = 0,
    DO_PARAM_ID,
    DO_PARAM_POLARITY,
    DO_PARAM_FAULT_STATE,
    DO_PARAM_COUNT
} do_param_t;

typedef enum
{
    TC_PARAM_NAME = 0,
    TC_PARAM_ID,
    TC_PARAM_TC_TYPE,
    TC_PARAM_UNIT,
    TC_PARAM_CJC_ENABLED,
    TC_PARAM_FILTER_MS,
    TC_PARAM_FAULT_STATE,
    TC_PARAM_FAULT_VALUE_C10,
    TC_PARAM_COUNT
} tc_param_t;

typedef enum
{
    AI_PARAM_NAME = 0,
    AI_PARAM_ID,
    AI_PARAM_INPUT_MODE,
    AI_PARAM_FILTER_MS,
    AI_PARAM_SCALE_NUM,
    AI_PARAM_SCALE_DEN,
    AI_PARAM_OFFSET,
    AI_PARAM_FAULT_STATE,
    AI_PARAM_FAULT_VALUE,
    AI_PARAM_COUNT
} ai_param_t;

typedef enum
{
    AO_PARAM_NAME = 0,
    AO_PARAM_ID,
    AO_PARAM_OUTPUT_MODE,
    AO_PARAM_FAULT_STATE,
    AO_PARAM_FAULT_VALUE,
    AO_PARAM_COUNT
} ao_param_t;

typedef enum
{
    PCNT_PARAM_NAME = 0,
    PCNT_PARAM_ID,
    PCNT_PARAM_MODE,
    PCNT_PARAM_EDGE,
    PCNT_PARAM_LIMIT,
    PCNT_PARAM_RESET_ON_READ,
    PCNT_PARAM_COUNT
} pcnt_param_t;

typedef enum
{
    PWM_PARAM_NAME = 0,
    PWM_PARAM_ID,
    PWM_PARAM_PERIOD_US,
    PWM_PARAM_DUTY_PERMILLE,
    PWM_PARAM_FAULT_STATE,
    PWM_PARAM_FAULT_DUTY_PERMILLE,
    PWM_PARAM_COUNT
} pwm_param_t;

typedef enum
{
    SYSTEM_PARAM_CANOPEN_NODE_ID = 0,
    SYSTEM_PARAM_CAN_BITRATE,
    SYSTEM_PARAM_HEARTBEAT_MS,
    SYSTEM_PARAM_SYNC_WINDOW_US,
    SYSTEM_PARAM_NMT_STARTUP,
    SYSTEM_PARAM_PRODUCER_EMCY_COB_ID,
    SYSTEM_PARAM_COUNT
} system_param_t;

/* Tagged value. The producer fills in the union arm appropriate to the
 * param's field type — the dispatcher trusts the caller for typing
 * (cheap; range/enum validation happens inside config_set_<type>()).
 *
 *   .u : unsigned integers, enums, booleans encoded as 0/1
 *   .i : signed integers (fault_value_c10, offset)
 *   .s : string fields (name) — must be NUL-terminated; truncated to
 *        CONFIG_NAME_LEN-1 chars before storage */
typedef union
{
    uint32_t u;
    int32_t  i;
    bool     b;
    char     s[CONFIG_NAME_LEN];
} eeprom_value_t;

/* ----- Public API -------------------------------------------------- */

/* Initialise the manager. MUST be called before vTaskStartScheduler.
 * Synchronously calls config_init() so the cache is populated by the
 * time other tasks start ("runs first on boot"). Then creates the
 * request queue and spawns the worker task at low priority.
 *
 * Returns the CONFIG_OK / CONFIG_ERR_* result from config_init so the
 * caller can log it. Even on storage errors the cache is loaded with
 * factory defaults and the manager task still spawns. */
config_status_t eeprom_manager_init (void);

/* Queue a piecewise change. Returns true on enqueue, false if the
 * queue is full or the manager isn't initialised. Validation of the
 * (type, param, value) tuple happens inside the manager task — a bad
 * tuple is logged and dropped; the caller does not learn about it
 * synchronously.
 *
 *   type   one of EEPROM_TYPE_*
 *   item   index for IO types (channel number); ignored for SYSTEM
 *   param  per-type field id (cast appropriate <type>_param_t to uint16)
 *   value  union arm matching the param's underlying field type */
bool QueueConfigChange (eeprom_config_type_t type,
                        uint8_t              item,
                        uint16_t             param,
                        eeprom_value_t       value);

/* Queue a monolithic commit: the cache has already been mutated by the
 * caller (e.g. config_import_json) and just needs to be persisted.
 * Returns true on enqueue, false if the queue is full. */
bool QueueConfigCommit (void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_EEPROM_MANAGER_H */
