/*****************************************************************************
 * Module:  config_json
 * Purpose: Implementation of the JSON wrapper declared in config_json.h.
 *
 *          Design notes:
 *            - Records carry an explicit `"channel"` field that names
 *              the cache index they target. Array position is purely
 *              presentational — operators don't have to ship leading
 *              nulls to address a high-indexed record.
 *            - Duplicate `channel` values within the same array are
 *              rejected after the first (first wins). Forces an
 *              operator typo to surface in `first_error` rather than
 *              silently last-writes-wins.
 *            - Enums encode as strings (case-sensitive) on export.
 *              Imports accept either the string form or the matching
 *              integer (tool-friendly).
 *            - Partial-field updates: when applying a JSON record, we
 *              start from `config_get_*` (the current cached value),
 *              patch only the fields present in the JSON, then call
 *              `config_set_*` so validation happens in the canonical
 *              setter path. Missing fields preserve their cached value.
 *            - `null` array entries are accepted as silent no-ops to
 *              keep operator JSON forgiving — they carry no addressing
 *              information so they touch nothing.
 *            - Unknown top-level keys are tolerated and counted —
 *              forward-compat at the JSON level mirrors the TLV layer's
 *              "skip and continue" property.
 *****************************************************************************/

#include "application/config_json.h"

#include "application/config.h"
#include "application/config_defaults.h"
#include "application/config_limits.h"
#include "application/config_types.h"

#include "cJSON.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 * Enum string tables
 * =================================================================== */

#define ENUM_STR(name)                        \
    static const char * name##_to_str(int v); \
    static bool         name##_from_json(const cJSON * j, int * out)

ENUM_STR(fault_state);
ENUM_STR(di_polarity);
ENUM_STR(do_polarity);
ENUM_STR(tc_type);
ENUM_STR(tc_unit);
ENUM_STR(ai_input_mode);
ENUM_STR(ao_output_mode);
ENUM_STR(pcnt_mode);
ENUM_STR(pcnt_edge);
ENUM_STR(can_bitrate);
ENUM_STR(nmt_startup);

static const char *
fault_state_to_str (int v)
{
    switch ((fault_state_t)v)
    {
        case FAULT_STATE_HOLD:
            return "HOLD";
        case FAULT_STATE_LOW:
            return "LOW";
        case FAULT_STATE_HIGH:
            return "HIGH";
        default:
            return NULL;
    }
}
static const char *
di_polarity_to_str (int v)
{
    switch ((di_polarity_t)v)
    {
        case DI_POLARITY_ACTIVE_HIGH:
            return "ACTIVE_HIGH";
        case DI_POLARITY_ACTIVE_LOW:
            return "ACTIVE_LOW";
        default:
            return NULL;
    }
}
static const char *
do_polarity_to_str (int v)
{
    switch ((do_polarity_t)v)
    {
        case DO_POLARITY_ACTIVE_HIGH:
            return "ACTIVE_HIGH";
        case DO_POLARITY_ACTIVE_LOW:
            return "ACTIVE_LOW";
        default:
            return NULL;
    }
}
static const char *
tc_type_to_str (int v)
{
    switch ((tc_type_t)v)
    {
        case TC_TYPE_K:
            return "K";
        case TC_TYPE_J:
            return "J";
        case TC_TYPE_T:
            return "T";
        case TC_TYPE_N:
            return "N";
        case TC_TYPE_S:
            return "S";
        case TC_TYPE_R:
            return "R";
        case TC_TYPE_B:
            return "B";
        case TC_TYPE_E:
            return "E";
        default:
            return NULL;
    }
}
static const char *
tc_unit_to_str (int v)
{
    switch ((tc_unit_t)v)
    {
        case TC_UNIT_CELSIUS:
            return "CELSIUS";
        case TC_UNIT_FAHRENHEIT:
            return "FAHRENHEIT";
        default:
            return NULL;
    }
}
static const char *
ai_input_mode_to_str (int v)
{
    switch ((ai_input_mode_t)v)
    {
        case AI_INPUT_MODE_VOLTAGE_0_10V:
            return "VOLTAGE_0_10V";
        case AI_INPUT_MODE_CURRENT_4_20MA_2W:
            return "CURRENT_4_20MA_2W";
        case AI_INPUT_MODE_CURRENT_4_20MA_3W:
            return "CURRENT_4_20MA_3W";
        default:
            return NULL;
    }
}
static const char *
ao_output_mode_to_str (int v)
{
    switch ((ao_output_mode_t)v)
    {
        case AO_OUTPUT_MODE_VOLTAGE_0_10V:
            return "VOLTAGE_0_10V";
        case AO_OUTPUT_MODE_CURRENT_4_20MA:
            return "CURRENT_4_20MA";
        default:
            return NULL;
    }
}
static const char *
pcnt_mode_to_str (int v)
{
    switch ((pcnt_mode_t)v)
    {
        case PCNT_MODE_COUNTER:
            return "COUNTER";
        case PCNT_MODE_FREQUENCY:
            return "FREQUENCY";
        default:
            return NULL;
    }
}
static const char *
pcnt_edge_to_str (int v)
{
    switch ((pcnt_edge_t)v)
    {
        case PCNT_EDGE_RISING:
            return "RISING";
        case PCNT_EDGE_FALLING:
            return "FALLING";
        case PCNT_EDGE_BOTH:
            return "BOTH";
        default:
            return NULL;
    }
}
static const char *
can_bitrate_to_str (int v)
{
    switch ((can_bitrate_t)v)
    {
        case CAN_BITRATE_125K:
            return "125K";
        case CAN_BITRATE_250K:
            return "250K";
        case CAN_BITRATE_500K:
            return "500K";
        case CAN_BITRATE_1M:
            return "1M";
        default:
            return NULL;
    }
}
static const char *
nmt_startup_to_str (int v)
{
    switch ((nmt_startup_t)v)
    {
        case NMT_STARTUP_WAIT:
            return "WAIT";
        case NMT_STARTUP_AUTOSTART:
            return "AUTOSTART";
        default:
            return NULL;
    }
}

/* enum_from_json: accept either string (case-sensitive match) or number.
 * Returns true on success and writes the resolved int value to *out. */
static bool
enum_from_json (const cJSON * j,
                int *         out,
                int           count,
                const char * (*to_str)(int))
{
    if (j == NULL || out == NULL || to_str == NULL)
    {
        return false;
    }
    if (cJSON_IsNumber(j))
    {
        const int v = (int)j->valuedouble;
        if (v < 0 || v >= count)
        {
            return false;
        }
        *out = v;
        return true;
    }
    if (cJSON_IsString(j))
    {
        const char * s = j->valuestring;
        for (int v = 0; v < count; ++v)
        {
            const char * t = to_str(v);
            if (t != NULL && strcmp(s, t) == 0)
            {
                *out = v;
                return true;
            }
        }
    }
    return false;
}

#define DEFINE_FROM(name, count_macro)                                  \
    static bool name##_from_json(const cJSON * j, int * out)            \
    {                                                                   \
        return enum_from_json(j, out, (int)count_macro, name##_to_str); \
    }

DEFINE_FROM(fault_state, FAULT_STATE_COUNT)
DEFINE_FROM(di_polarity, DI_POLARITY_COUNT)
DEFINE_FROM(do_polarity, DO_POLARITY_COUNT)
DEFINE_FROM(tc_type, TC_TYPE_COUNT)
DEFINE_FROM(tc_unit, TC_UNIT_COUNT)
DEFINE_FROM(ai_input_mode, AI_INPUT_MODE_COUNT)
DEFINE_FROM(ao_output_mode, AO_OUTPUT_MODE_COUNT)
DEFINE_FROM(pcnt_mode, PCNT_MODE_COUNT)
DEFINE_FROM(pcnt_edge, PCNT_EDGE_COUNT)
DEFINE_FROM(can_bitrate, CAN_BITRATE_COUNT)
DEFINE_FROM(nmt_startup, NMT_STARTUP_COUNT)

/* ===================================================================
 * Report helpers
 * =================================================================== */

static void
report_reset (config_import_report_t * r)
{
    if (r == NULL)
    {
        return;
    }
    r->accepted       = 0;
    r->rejected       = 0;
    r->unknown_keys   = 0;
    r->malformed      = 0;
    r->first_error[0] = '\0';
}

static void
report_first_error (config_import_report_t * r, const char * fmt, ...)
{
    if (r == NULL || r->first_error[0] != '\0')
    {
        return; /* keep the FIRST error, ignore subsequent */
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(r->first_error, sizeof(r->first_error), fmt, ap);
    va_end(ap);
}

/* ===================================================================
 * Field patchers — read fields from a JSON object onto a typed struct
 *
 * Each helper:
 *   - Reads the named field from the JSON object if present
 *   - On success, writes to the struct
 *   - On format error (wrong type, out-of-range enum string), returns false
 *   - Missing field is NOT an error; the struct keeps its current value
 * =================================================================== */

static bool
patch_string (cJSON * obj, const char * key, char * dst, size_t cap)
{
    cJSON * j = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (j == NULL || cJSON_IsNull(j))
    {
        return true; /* missing — keep cached */
    }
    if (!cJSON_IsString(j) || j->valuestring == NULL)
    {
        return false;
    }
    const size_t n = strlen(j->valuestring);
    if (n >= cap)
    {
        return false;
    }
    memcpy(dst, j->valuestring, n);
    dst[n] = '\0';
    /* zero-fill the rest so the cache representation stays tidy */
    if (n + 1 < cap)
    {
        memset(dst + n + 1, 0, cap - n - 1);
    }
    return true;
}

static bool
patch_uint (cJSON * obj, const char * key, uint32_t * dst, uint32_t max_value)
{
    cJSON * j = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (j == NULL || cJSON_IsNull(j))
    {
        return true;
    }
    if (!cJSON_IsNumber(j))
    {
        return false;
    }
    const double v = j->valuedouble;
    if (v < 0.0 || v > (double)max_value)
    {
        return false;
    }
    *dst = (uint32_t)v;
    return true;
}

static bool
patch_int (cJSON *      obj,
           const char * key,
           int32_t *    dst,
           int32_t      min_value,
           int32_t      max_value)
{
    cJSON * j = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (j == NULL || cJSON_IsNull(j))
    {
        return true;
    }
    if (!cJSON_IsNumber(j))
    {
        return false;
    }
    const double v = j->valuedouble;
    if (v < (double)min_value || v > (double)max_value)
    {
        return false;
    }
    *dst = (int32_t)v;
    return true;
}

static bool
patch_bool (cJSON * obj, const char * key, bool * dst)
{
    cJSON * j = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (j == NULL || cJSON_IsNull(j))
    {
        return true;
    }
    if (!cJSON_IsBool(j))
    {
        return false;
    }
    *dst = cJSON_IsTrue(j);
    return true;
}

#define PATCH_ENUM(obj, key, dst_field, from_fn)                     \
    do                                                               \
    {                                                                \
        cJSON * _j = cJSON_GetObjectItemCaseSensitive((obj), (key)); \
        if (_j != NULL && !cJSON_IsNull(_j))                         \
        {                                                            \
            int _v;                                                  \
            if (!from_fn(_j, &_v))                                   \
            {                                                        \
                return false;                                        \
            }                                                        \
            (dst_field) = _v;                                        \
        }                                                            \
    } while (0)

/* ===================================================================
 * Export helpers
 * =================================================================== */

static cJSON *
build_di (uint8_t channel, const di_config_t * di)
{
    cJSON * o = cJSON_CreateObject();
    if (o == NULL)
    {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "channel", (double)channel);
    cJSON_AddStringToObject(o, "name", di->name);
    cJSON_AddNumberToObject(o, "id", (double)di->id);
    cJSON_AddNumberToObject(o, "debounce_ms", (double)di->debounce_ms);
    cJSON_AddStringToObject(
        o, "polarity", di_polarity_to_str((int)di->polarity));
    cJSON_AddStringToObject(
        o, "fault_state", fault_state_to_str((int)di->fault_state));
    cJSON_AddBoolToObject(o, "interrupt_enabled", di->interrupt_enabled);
    return o;
}

static cJSON *
build_do (uint8_t channel, const do_config_t * d)
{
    cJSON * o = cJSON_CreateObject();
    if (o == NULL)
    {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "channel", (double)channel);
    cJSON_AddStringToObject(o, "name", d->name);
    cJSON_AddNumberToObject(o, "id", (double)d->id);
    cJSON_AddStringToObject(
        o, "polarity", do_polarity_to_str((int)d->polarity));
    cJSON_AddStringToObject(
        o, "fault_state", fault_state_to_str((int)d->fault_state));
    return o;
}

static cJSON *
build_tc (uint8_t channel, const tc_config_t * t)
{
    cJSON * o = cJSON_CreateObject();
    if (o == NULL)
    {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "channel", (double)channel);
    cJSON_AddStringToObject(o, "name", t->name);
    cJSON_AddNumberToObject(o, "id", (double)t->id);
    cJSON_AddStringToObject(o, "tc_type", tc_type_to_str((int)t->tc_type));
    cJSON_AddStringToObject(o, "unit", tc_unit_to_str((int)t->unit));
    cJSON_AddBoolToObject(o, "cjc_enabled", t->cjc_enabled);
    cJSON_AddNumberToObject(o, "filter_ms", (double)t->filter_ms);
    cJSON_AddStringToObject(
        o, "fault_state", fault_state_to_str((int)t->fault_state));
    cJSON_AddNumberToObject(o, "fault_value_c10", (double)t->fault_value_c10);
    return o;
}

static cJSON *
build_ai (uint8_t channel, const ai_config_t * a)
{
    cJSON * o = cJSON_CreateObject();
    if (o == NULL)
    {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "channel", (double)channel);
    cJSON_AddStringToObject(o, "name", a->name);
    cJSON_AddNumberToObject(o, "id", (double)a->id);
    cJSON_AddStringToObject(
        o, "input_mode", ai_input_mode_to_str((int)a->input_mode));
    cJSON_AddNumberToObject(o, "filter_ms", (double)a->filter_ms);
    cJSON_AddNumberToObject(o, "scale_num", (double)a->scale_num);
    cJSON_AddNumberToObject(o, "scale_den", (double)a->scale_den);
    cJSON_AddNumberToObject(o, "offset", (double)a->offset);
    cJSON_AddStringToObject(
        o, "fault_state", fault_state_to_str((int)a->fault_state));
    cJSON_AddNumberToObject(o, "fault_value", (double)a->fault_value);
    return o;
}

static cJSON *
build_ao (uint8_t channel, const ao_config_t * a)
{
    cJSON * o = cJSON_CreateObject();
    if (o == NULL)
    {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "channel", (double)channel);
    cJSON_AddStringToObject(o, "name", a->name);
    cJSON_AddNumberToObject(o, "id", (double)a->id);
    cJSON_AddStringToObject(
        o, "output_mode", ao_output_mode_to_str((int)a->output_mode));
    cJSON_AddNumberToObject(o, "slew_per_s", (double)a->slew_per_s);
    cJSON_AddNumberToObject(o, "scale_num", (double)a->scale_num);
    cJSON_AddNumberToObject(o, "scale_den", (double)a->scale_den);
    cJSON_AddNumberToObject(o, "offset", (double)a->offset);
    cJSON_AddStringToObject(
        o, "fault_state", fault_state_to_str((int)a->fault_state));
    cJSON_AddNumberToObject(o, "fault_value", (double)a->fault_value);
    return o;
}

static cJSON *
build_pcnt (uint8_t channel, const pcnt_config_t * p)
{
    cJSON * o = cJSON_CreateObject();
    if (o == NULL)
    {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "channel", (double)channel);
    cJSON_AddStringToObject(o, "name", p->name);
    cJSON_AddNumberToObject(o, "id", (double)p->id);
    cJSON_AddStringToObject(o, "mode", pcnt_mode_to_str((int)p->mode));
    cJSON_AddStringToObject(o, "edge", pcnt_edge_to_str((int)p->edge));
    cJSON_AddNumberToObject(o, "limit", (double)p->limit);
    cJSON_AddBoolToObject(o, "reset_on_read", p->reset_on_read);
    return o;
}

static cJSON *
build_pwm (uint8_t channel, const pwm_config_t * p)
{
    cJSON * o = cJSON_CreateObject();
    if (o == NULL)
    {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "channel", (double)channel);
    cJSON_AddStringToObject(o, "name", p->name);
    cJSON_AddNumberToObject(o, "id", (double)p->id);
    cJSON_AddNumberToObject(o, "period_us", (double)p->period_us);
    cJSON_AddNumberToObject(o, "duty_permille", (double)p->duty_permille);
    cJSON_AddStringToObject(
        o, "fault_state", fault_state_to_str((int)p->fault_state));
    cJSON_AddNumberToObject(
        o, "fault_duty_permille", (double)p->fault_duty_permille);
    return o;
}

static cJSON *
build_system (const system_config_t * s)
{
    cJSON * o = cJSON_CreateObject();
    if (o == NULL)
    {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "canopen_node_id", (double)s->canopen_node_id);
    cJSON_AddStringToObject(
        o, "can_bitrate", can_bitrate_to_str((int)s->can_bitrate));
    cJSON_AddNumberToObject(o, "heartbeat_ms", (double)s->heartbeat_ms);
    cJSON_AddNumberToObject(o, "sync_window_us", (double)s->sync_window_us);
    cJSON_AddStringToObject(
        o, "nmt_startup", nmt_startup_to_str((int)s->nmt_startup));
    cJSON_AddNumberToObject(
        o, "producer_emcy_cob_id", (double)s->producer_emcy_cob_id);
    return o;
}

/* ===================================================================
 * Apply helpers — patch a record and call config_set_*
 *
 * The "patch starting from current cache value" pattern means a JSON
 * record containing only the fields the operator wants to change
 * works correctly. Missing fields keep their cached value.
 * =================================================================== */

static bool
patch_di_record (cJSON * obj, di_config_t * di)
{
    if (!patch_string(obj, "name", di->name, sizeof(di->name)))
    {
        return false;
    }
    uint32_t u = di->id;
    if (!patch_uint(obj, "id", &u, UINT16_MAX))
    {
        return false;
    }
    di->id = (uint16_t)u;
    u      = di->debounce_ms;
    if (!patch_uint(obj, "debounce_ms", &u, UINT16_MAX))
    {
        return false;
    }
    di->debounce_ms = (uint16_t)u;
    PATCH_ENUM(obj, "polarity", di->polarity, di_polarity_from_json);
    PATCH_ENUM(obj, "fault_state", di->fault_state, fault_state_from_json);
    if (!patch_bool(obj, "interrupt_enabled", &di->interrupt_enabled))
    {
        return false;
    }
    return true;
}

static bool
patch_do_record (cJSON * obj, do_config_t * d)
{
    if (!patch_string(obj, "name", d->name, sizeof(d->name)))
    {
        return false;
    }
    uint32_t u = d->id;
    if (!patch_uint(obj, "id", &u, UINT16_MAX))
    {
        return false;
    }
    d->id = (uint16_t)u;
    PATCH_ENUM(obj, "polarity", d->polarity, do_polarity_from_json);
    PATCH_ENUM(obj, "fault_state", d->fault_state, fault_state_from_json);
    return true;
}

static bool
patch_tc_record (cJSON * obj, tc_config_t * t)
{
    if (!patch_string(obj, "name", t->name, sizeof(t->name)))
    {
        return false;
    }
    uint32_t u = t->id;
    if (!patch_uint(obj, "id", &u, UINT16_MAX))
    {
        return false;
    }
    t->id = (uint16_t)u;
    PATCH_ENUM(obj, "tc_type", t->tc_type, tc_type_from_json);
    PATCH_ENUM(obj, "unit", t->unit, tc_unit_from_json);
    if (!patch_bool(obj, "cjc_enabled", &t->cjc_enabled))
    {
        return false;
    }
    u = t->filter_ms;
    if (!patch_uint(obj, "filter_ms", &u, UINT16_MAX))
    {
        return false;
    }
    t->filter_ms = (uint16_t)u;
    PATCH_ENUM(obj, "fault_state", t->fault_state, fault_state_from_json);
    int32_t i = t->fault_value_c10;
    if (!patch_int(obj, "fault_value_c10", &i, INT16_MIN, INT16_MAX))
    {
        return false;
    }
    t->fault_value_c10 = (int16_t)i;
    return true;
}

static bool
patch_ai_record (cJSON * obj, ai_config_t * a)
{
    if (!patch_string(obj, "name", a->name, sizeof(a->name)))
    {
        return false;
    }
    uint32_t u = a->id;
    if (!patch_uint(obj, "id", &u, UINT16_MAX))
    {
        return false;
    }
    a->id = (uint16_t)u;
    PATCH_ENUM(obj, "input_mode", a->input_mode, ai_input_mode_from_json);
    u = a->filter_ms;
    if (!patch_uint(obj, "filter_ms", &u, UINT16_MAX))
    {
        return false;
    }
    a->filter_ms = (uint16_t)u;
    if (!patch_int(obj, "scale_num", &a->scale_num, INT32_MIN, INT32_MAX))
    {
        return false;
    }
    if (!patch_int(obj, "scale_den", &a->scale_den, INT32_MIN, INT32_MAX))
    {
        return false;
    }
    if (!patch_int(obj, "offset", &a->offset, INT32_MIN, INT32_MAX))
    {
        return false;
    }
    PATCH_ENUM(obj, "fault_state", a->fault_state, fault_state_from_json);
    if (!patch_int(obj, "fault_value", &a->fault_value, INT32_MIN, INT32_MAX))
    {
        return false;
    }
    return true;
}

static bool
patch_ao_record (cJSON * obj, ao_config_t * a)
{
    if (!patch_string(obj, "name", a->name, sizeof(a->name)))
    {
        return false;
    }
    uint32_t u = a->id;
    if (!patch_uint(obj, "id", &u, UINT16_MAX))
    {
        return false;
    }
    a->id = (uint16_t)u;
    PATCH_ENUM(obj, "output_mode", a->output_mode, ao_output_mode_from_json);
    u = a->slew_per_s;
    if (!patch_uint(obj, "slew_per_s", &u, UINT16_MAX))
    {
        return false;
    }
    a->slew_per_s = (uint16_t)u;
    if (!patch_int(obj, "scale_num", &a->scale_num, INT32_MIN, INT32_MAX))
    {
        return false;
    }
    if (!patch_int(obj, "scale_den", &a->scale_den, INT32_MIN, INT32_MAX))
    {
        return false;
    }
    if (!patch_int(obj, "offset", &a->offset, INT32_MIN, INT32_MAX))
    {
        return false;
    }
    PATCH_ENUM(obj, "fault_state", a->fault_state, fault_state_from_json);
    if (!patch_int(obj, "fault_value", &a->fault_value, INT32_MIN, INT32_MAX))
    {
        return false;
    }
    return true;
}

static bool
patch_pcnt_record (cJSON * obj, pcnt_config_t * p)
{
    if (!patch_string(obj, "name", p->name, sizeof(p->name)))
    {
        return false;
    }
    uint32_t u = p->id;
    if (!patch_uint(obj, "id", &u, UINT16_MAX))
    {
        return false;
    }
    p->id = (uint16_t)u;
    PATCH_ENUM(obj, "mode", p->mode, pcnt_mode_from_json);
    PATCH_ENUM(obj, "edge", p->edge, pcnt_edge_from_json);
    if (!patch_uint(obj, "limit", &p->limit, UINT32_MAX))
    {
        return false;
    }
    if (!patch_bool(obj, "reset_on_read", &p->reset_on_read))
    {
        return false;
    }
    return true;
}

static bool
patch_pwm_record (cJSON * obj, pwm_config_t * p)
{
    if (!patch_string(obj, "name", p->name, sizeof(p->name)))
    {
        return false;
    }
    uint32_t u = p->id;
    if (!patch_uint(obj, "id", &u, UINT16_MAX))
    {
        return false;
    }
    p->id = (uint16_t)u;
    if (!patch_uint(obj, "period_us", &p->period_us, UINT32_MAX))
    {
        return false;
    }
    u = p->duty_permille;
    if (!patch_uint(obj, "duty_permille", &u, UINT16_MAX))
    {
        return false;
    }
    p->duty_permille = (uint16_t)u;
    PATCH_ENUM(obj, "fault_state", p->fault_state, fault_state_from_json);
    u = p->fault_duty_permille;
    if (!patch_uint(obj, "fault_duty_permille", &u, UINT16_MAX))
    {
        return false;
    }
    p->fault_duty_permille = (uint16_t)u;
    return true;
}

static bool
patch_system_record (cJSON * obj, system_config_t * s)
{
    uint32_t u = s->canopen_node_id;
    if (!patch_uint(obj, "canopen_node_id", &u, UINT8_MAX))
    {
        return false;
    }
    s->canopen_node_id = (uint8_t)u;
    PATCH_ENUM(obj, "can_bitrate", s->can_bitrate, can_bitrate_from_json);
    u = s->heartbeat_ms;
    if (!patch_uint(obj, "heartbeat_ms", &u, UINT16_MAX))
    {
        return false;
    }
    s->heartbeat_ms = (uint16_t)u;
    if (!patch_uint(obj, "sync_window_us", &s->sync_window_us, UINT32_MAX))
    {
        return false;
    }
    PATCH_ENUM(obj, "nmt_startup", s->nmt_startup, nmt_startup_from_json);
    if (!patch_uint(
            obj, "producer_emcy_cob_id", &s->producer_emcy_cob_id, UINT32_MAX))
    {
        return false;
    }
    return true;
}

/* ===================================================================
 * Array dispatch — route each record to config_set_* keyed on its
 * explicit "channel" field. Records without a valid channel are
 * rejected; duplicate channels in the same array are rejected after
 * the first (the first wins). null entries are accepted as silent
 * no-ops to keep operator JSON forgiving.
 * =================================================================== */

#define APPLY_ARRAY(arr_name, ctype, count_macro, patcher, getter, setter)     \
    do                                                                         \
    {                                                                          \
        cJSON * _arr = cJSON_GetObjectItemCaseSensitive(root, arr_name);       \
        if (_arr == NULL || cJSON_IsNull(_arr))                                \
        {                                                                      \
            break;                                                             \
        }                                                                      \
        if (!cJSON_IsArray(_arr))                                              \
        {                                                                      \
            report_first_error(report, "%s: not an array", arr_name);          \
            report->malformed++;                                               \
            break;                                                             \
        }                                                                      \
        uint32_t  _seen = 0u;                                                  \
        const int _n    = cJSON_GetArraySize(_arr);                            \
        for (int _i = 0; _i < _n; ++_i)                                        \
        {                                                                      \
            cJSON * _item = cJSON_GetArrayItem(_arr, _i);                      \
            if (_item == NULL || cJSON_IsNull(_item))                          \
            {                                                                  \
                continue;                                                      \
            }                                                                  \
            if (!cJSON_IsObject(_item))                                        \
            {                                                                  \
                report_first_error(                                            \
                    report, "%s[%d]: not an object", arr_name, _i);            \
                report->malformed++;                                           \
                continue;                                                      \
            }                                                                  \
            cJSON * _ch = cJSON_GetObjectItemCaseSensitive(_item, "channel");  \
            if (_ch == NULL || !cJSON_IsNumber(_ch))                           \
            {                                                                  \
                report_first_error(report,                                     \
                                   "%s[%d]: missing or non-integer 'channel'", \
                                   arr_name,                                   \
                                   _i);                                        \
                report->rejected++;                                            \
                continue;                                                      \
            }                                                                  \
            const double _chv = _ch->valuedouble;                              \
            if (_chv < 0.0 || _chv >= (double)(count_macro))                   \
            {                                                                  \
                report_first_error(report,                                     \
                                   "%s[%d]: channel %d out of range",          \
                                   arr_name,                                   \
                                   _i,                                         \
                                   (int)_chv);                                 \
                report->rejected++;                                            \
                continue;                                                      \
            }                                                                  \
            const uint8_t _idx = (uint8_t)_chv;                                \
            if ((_seen & (1u << _idx)) != 0u)                                  \
            {                                                                  \
                report_first_error(report,                                     \
                                   "%s[%d]: duplicate channel %u",             \
                                   arr_name,                                   \
                                   _i,                                         \
                                   (unsigned)_idx);                            \
                report->rejected++;                                            \
                continue;                                                      \
            }                                                                  \
            _seen |= (1u << _idx);                                             \
            ctype _v;                                                          \
            if (getter(_idx, &_v) != CONFIG_OK)                                \
            {                                                                  \
                report->rejected++;                                            \
                continue;                                                      \
            }                                                                  \
            if (!patcher(_item, &_v))                                          \
            {                                                                  \
                report_first_error(report,                                     \
                                   "%s[ch=%u]: malformed field",               \
                                   arr_name,                                   \
                                   (unsigned)_idx);                            \
                report->rejected++;                                            \
                continue;                                                      \
            }                                                                  \
            if (setter(_idx, &_v) != CONFIG_OK)                                \
            {                                                                  \
                report_first_error(report,                                     \
                                   "%s[ch=%u]: setter rejected",               \
                                   arr_name,                                   \
                                   (unsigned)_idx);                            \
                report->rejected++;                                            \
                continue;                                                      \
            }                                                                  \
            report->accepted++;                                                \
        }                                                                      \
    } while (0)

/* ===================================================================
 * Public API
 * =================================================================== */

config_status_t
config_export_json (char * buf, size_t cap, size_t * written)
{
    if (buf == NULL || written == NULL || cap == 0)
    {
        return CONFIG_ERR_INVALID;
    }

    cJSON * root = cJSON_CreateObject();
    if (root == NULL)
    {
        return CONFIG_ERR_INTERNAL;
    }
    cJSON_AddNumberToObject(root, "format", 2.0);

    /* system */
    {
        system_config_t s;
        if (config_get_system(&s) == CONFIG_OK)
        {
            cJSON * obj = build_system(&s);
            if (obj == NULL)
            {
                cJSON_Delete(root);
                return CONFIG_ERR_INTERNAL;
            }
            cJSON_AddItemToObject(root, "system", obj);
        }
    }

#define EMIT_ARRAY(name, ctype, count_macro, getter, builder) \
    do                                                        \
    {                                                         \
        cJSON * _arr = cJSON_CreateArray();                   \
        if (_arr == NULL)                                     \
        {                                                     \
            cJSON_Delete(root);                               \
            return CONFIG_ERR_INTERNAL;                       \
        }                                                     \
        for (uint8_t _i = 0; _i < (count_macro); ++_i)        \
        {                                                     \
            ctype _v;                                         \
            if (getter(_i, &_v) != CONFIG_OK)                 \
            {                                                 \
                continue;                                     \
            }                                                 \
            cJSON * _o = builder(_i, &_v);                    \
            if (_o == NULL)                                   \
            {                                                 \
                cJSON_Delete(root);                           \
                return CONFIG_ERR_INTERNAL;                   \
            }                                                 \
            cJSON_AddItemToArray(_arr, _o);                   \
        }                                                     \
        cJSON_AddItemToObject(root, name, _arr);              \
    } while (0)

    EMIT_ARRAY("di", di_config_t, CONFIG_NUM_DI, config_get_di, build_di);
    EMIT_ARRAY("do", do_config_t, CONFIG_NUM_DO, config_get_do, build_do);
    EMIT_ARRAY("tc", tc_config_t, CONFIG_NUM_TC, config_get_tc, build_tc);
    EMIT_ARRAY("ai", ai_config_t, CONFIG_NUM_AI, config_get_ai, build_ai);
    EMIT_ARRAY("ao", ao_config_t, CONFIG_NUM_AO, config_get_ao, build_ao);
    EMIT_ARRAY(
        "pcnt", pcnt_config_t, CONFIG_NUM_PCNT, config_get_pcnt, build_pcnt);
    EMIT_ARRAY("pwm", pwm_config_t, CONFIG_NUM_PWM, config_get_pwm, build_pwm);

#undef EMIT_ARRAY

    char * out = cJSON_PrintBuffered(root, 1024, 1 /* formatted */);
    cJSON_Delete(root);
    if (out == NULL)
    {
        return CONFIG_ERR_INTERNAL;
    }
    const size_t len = strlen(out);
    if (len + 1u > cap)
    {
        cJSON_free(out);
        return CONFIG_ERR_TOO_LARGE;
    }
    memcpy(buf, out, len + 1u);
    *written = len;
    cJSON_free(out);
    return CONFIG_OK;
}

config_status_t
config_import_json (const char *             json,
                    size_t                   len,
                    config_import_report_t * report)
{
    config_import_report_t local_report;
    if (report == NULL)
    {
        report = &local_report;
    }
    report_reset(report);

    if (json == NULL)
    {
        return CONFIG_ERR_INVALID;
    }

    cJSON * root = cJSON_ParseWithLength(json, len);
    if (root == NULL)
    {
        return CONFIG_ERR_CODEC;
    }
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return CONFIG_ERR_INVALID;
    }

    /* Walk top-level keys. Known ones get dispatched; unknown ones get
     * counted in unknown_keys (forward compat). */
    cJSON * child = NULL;
    cJSON_ArrayForEach(child, root)
    {
        const char * key = child->string;
        if (key == NULL)
        {
            continue;
        }
        if (strcmp(key, "format") == 0)
        {
            continue; /* format version: accept, no behaviour yet */
        }
        if (strcmp(key, "system") == 0)
        {
            if (!cJSON_IsObject(child))
            {
                report_first_error(report, "system: not an object");
                report->malformed++;
                continue;
            }
            system_config_t s;
            if (config_get_system(&s) != CONFIG_OK)
            {
                report->rejected++;
                continue;
            }
            if (!patch_system_record(child, &s))
            {
                report_first_error(report, "system: malformed field");
                report->rejected++;
                continue;
            }
            if (config_set_system(&s) != CONFIG_OK)
            {
                report_first_error(report, "system: setter rejected");
                report->rejected++;
                continue;
            }
            report->accepted++;
            continue;
        }
        if (strcmp(key, "di") == 0)
        {
            APPLY_ARRAY("di",
                        di_config_t,
                        CONFIG_NUM_DI,
                        patch_di_record,
                        config_get_di,
                        config_set_di);
            continue;
        }
        if (strcmp(key, "do") == 0)
        {
            APPLY_ARRAY("do",
                        do_config_t,
                        CONFIG_NUM_DO,
                        patch_do_record,
                        config_get_do,
                        config_set_do);
            continue;
        }
        if (strcmp(key, "tc") == 0)
        {
            APPLY_ARRAY("tc",
                        tc_config_t,
                        CONFIG_NUM_TC,
                        patch_tc_record,
                        config_get_tc,
                        config_set_tc);
            continue;
        }
        if (strcmp(key, "ai") == 0)
        {
            APPLY_ARRAY("ai",
                        ai_config_t,
                        CONFIG_NUM_AI,
                        patch_ai_record,
                        config_get_ai,
                        config_set_ai);
            continue;
        }
        if (strcmp(key, "ao") == 0)
        {
            APPLY_ARRAY("ao",
                        ao_config_t,
                        CONFIG_NUM_AO,
                        patch_ao_record,
                        config_get_ao,
                        config_set_ao);
            continue;
        }
        if (strcmp(key, "pcnt") == 0)
        {
            APPLY_ARRAY("pcnt",
                        pcnt_config_t,
                        CONFIG_NUM_PCNT,
                        patch_pcnt_record,
                        config_get_pcnt,
                        config_set_pcnt);
            continue;
        }
        if (strcmp(key, "pwm") == 0)
        {
            APPLY_ARRAY("pwm",
                        pwm_config_t,
                        CONFIG_NUM_PWM,
                        patch_pwm_record,
                        config_get_pwm,
                        config_set_pwm);
            continue;
        }
        /* Unknown top-level key — tolerate, count, continue. */
        report->unknown_keys++;
    }

    cJSON_Delete(root);
    return CONFIG_OK;
}
