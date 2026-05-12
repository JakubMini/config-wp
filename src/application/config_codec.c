/*****************************************************************************
 * Module:  config_codec
 * Purpose: Implementation of the type-aware wire codecs. Field-by-field
 *          little-endian serialisation, no struct padding crosses the
 *          wire. See config_codec.h for the contract.
 *
 *          The pattern across every type:
 *
 *              encode_xxx: write fields in declaration order into a
 *                          cursor that advances by exactly the field
 *                          width. enums and bools are narrowed to u8
 *                          on the wire.
 *
 *              decode_xxx: mirror, reading the cursor forward by the
 *                          same widths.
 *
 *          Each function checks that the buffer is large enough up
 *          front and refuses to write/read past the end. Wire sizes
 *          are pinned by config_codec.h constants and verified at
 *          encode/decode time and again by unit tests.
 *****************************************************************************/

#include "application/config_codec.h"

#include <assert.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Little-endian byte helpers                                            */
/* --------------------------------------------------------------------- */

static void
put_u8 (uint8_t ** p, uint8_t v)
{
    *(*p)++ = v;
}

static void
put_u16 (uint8_t ** p, uint16_t v)
{
    (*p)[0] = (uint8_t)(v & 0xFFu);
    (*p)[1] = (uint8_t)((v >> 8) & 0xFFu);
    *p += 2;
}

static void
put_u32 (uint8_t ** p, uint32_t v)
{
    (*p)[0] = (uint8_t)(v & 0xFFu);
    (*p)[1] = (uint8_t)((v >> 8) & 0xFFu);
    (*p)[2] = (uint8_t)((v >> 16) & 0xFFu);
    (*p)[3] = (uint8_t)((v >> 24) & 0xFFu);
    *p += 4;
}

static void
put_i16 (uint8_t ** p, int16_t v)
{
    put_u16(p, (uint16_t)v);
}

static void
put_i32 (uint8_t ** p, int32_t v)
{
    put_u32(p, (uint32_t)v);
}

static void
put_bytes (uint8_t ** p, const void * src, size_t n)
{
    memcpy(*p, src, n);
    *p += n;
}

static uint8_t
get_u8 (const uint8_t ** p)
{
    return *(*p)++;
}

static uint16_t
get_u16 (const uint8_t ** p)
{
    uint16_t v = (uint16_t)(*p)[0] | ((uint16_t)(*p)[1] << 8);
    *p += 2;
    return v;
}

static uint32_t
get_u32 (const uint8_t ** p)
{
    uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8)
                 | ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return v;
}

static int16_t
get_i16 (const uint8_t ** p)
{
    return (int16_t)get_u16(p);
}

static int32_t
get_i32 (const uint8_t ** p)
{
    return (int32_t)get_u32(p);
}

static void
get_bytes (const uint8_t ** p, void * dst, size_t n)
{
    memcpy(dst, *p, n);
    *p += n;
}

/* Common pre-checks. */
static tlv_status_t
check_encode (const void * in, void * buf, size_t cap, size_t need)
{
    if (in == NULL || buf == NULL)
    {
        return TLV_ERR_BUF;
    }
    if (cap < need)
    {
        return TLV_ERR_NO_SPACE;
    }
    return TLV_OK;
}

static tlv_status_t
check_decode (const void * buf, void * out, size_t len, size_t need)
{
    if (buf == NULL || out == NULL)
    {
        return TLV_ERR_BUF;
    }
    if (len < need)
    {
        return TLV_ERR_TRUNCATED;
    }
    return TLV_OK;
}

/* --------------------------------------------------------------------- */
/* DI                                                                    */
/* --------------------------------------------------------------------- */

tlv_status_t
config_codec_encode_di (const di_config_t * in,
                        void *              buf,
                        size_t              cap,
                        size_t *            out_size)
{
    tlv_status_t st = check_encode(in, buf, cap, CONFIG_CODEC_DI_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    uint8_t * p = (uint8_t *)buf;
    put_bytes(&p, in->name, CONFIG_NAME_LEN);
    put_u16(&p, in->id);
    put_u16(&p, in->debounce_ms);
    put_u8(&p, (uint8_t)in->polarity);
    put_u8(&p, (uint8_t)in->fault_state);
    put_u8(&p, in->interrupt_enabled ? 1u : 0u);
    assert((size_t)(p - (uint8_t *)buf) == CONFIG_CODEC_DI_WIRE_SIZE);
    *out_size = CONFIG_CODEC_DI_WIRE_SIZE;
    return TLV_OK;
}

tlv_status_t
config_codec_decode_di (const void * buf, size_t len, di_config_t * out)
{
    tlv_status_t st = check_decode(buf, out, len, CONFIG_CODEC_DI_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    const uint8_t * p = (const uint8_t *)buf;
    get_bytes(&p, out->name, CONFIG_NAME_LEN);
    out->name[CONFIG_NAME_LEN - 1] = '\0'; /* enforce null termination */
    out->id                        = get_u16(&p);
    out->debounce_ms               = get_u16(&p);
    out->polarity                  = (di_polarity_t)get_u8(&p);
    out->fault_state               = (fault_state_t)get_u8(&p);
    out->interrupt_enabled         = get_u8(&p) != 0u;
    return TLV_OK;
}

/* --------------------------------------------------------------------- */
/* DO                                                                    */
/* --------------------------------------------------------------------- */

tlv_status_t
config_codec_encode_do (const do_config_t * in,
                        void *              buf,
                        size_t              cap,
                        size_t *            out_size)
{
    tlv_status_t st = check_encode(in, buf, cap, CONFIG_CODEC_DO_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    uint8_t * p = (uint8_t *)buf;
    put_bytes(&p, in->name, CONFIG_NAME_LEN);
    put_u16(&p, in->id);
    put_u8(&p, (uint8_t)in->polarity);
    put_u8(&p, (uint8_t)in->fault_state);
    assert((size_t)(p - (uint8_t *)buf) == CONFIG_CODEC_DO_WIRE_SIZE);
    *out_size = CONFIG_CODEC_DO_WIRE_SIZE;
    return TLV_OK;
}

tlv_status_t
config_codec_decode_do (const void * buf, size_t len, do_config_t * out)
{
    tlv_status_t st = check_decode(buf, out, len, CONFIG_CODEC_DO_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    const uint8_t * p = (const uint8_t *)buf;
    get_bytes(&p, out->name, CONFIG_NAME_LEN);
    out->name[CONFIG_NAME_LEN - 1] = '\0';
    out->id                        = get_u16(&p);
    out->polarity                  = (do_polarity_t)get_u8(&p);
    out->fault_state               = (fault_state_t)get_u8(&p);
    return TLV_OK;
}

/* --------------------------------------------------------------------- */
/* TC                                                                    */
/* --------------------------------------------------------------------- */

tlv_status_t
config_codec_encode_tc (const tc_config_t * in,
                        void *              buf,
                        size_t              cap,
                        size_t *            out_size)
{
    tlv_status_t st = check_encode(in, buf, cap, CONFIG_CODEC_TC_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    uint8_t * p = (uint8_t *)buf;
    put_bytes(&p, in->name, CONFIG_NAME_LEN);
    put_u16(&p, in->id);
    put_u8(&p, (uint8_t)in->tc_type);
    put_u8(&p, (uint8_t)in->unit);
    put_u8(&p, in->cjc_enabled ? 1u : 0u);
    put_u16(&p, in->filter_ms);
    put_u8(&p, (uint8_t)in->fault_state);
    put_i16(&p, in->fault_value_c10);
    assert((size_t)(p - (uint8_t *)buf) == CONFIG_CODEC_TC_WIRE_SIZE);
    *out_size = CONFIG_CODEC_TC_WIRE_SIZE;
    return TLV_OK;
}

tlv_status_t
config_codec_decode_tc (const void * buf, size_t len, tc_config_t * out)
{
    tlv_status_t st = check_decode(buf, out, len, CONFIG_CODEC_TC_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    const uint8_t * p = (const uint8_t *)buf;
    get_bytes(&p, out->name, CONFIG_NAME_LEN);
    out->name[CONFIG_NAME_LEN - 1] = '\0';
    out->id                        = get_u16(&p);
    out->tc_type                   = (tc_type_t)get_u8(&p);
    out->unit                      = (tc_unit_t)get_u8(&p);
    out->cjc_enabled               = get_u8(&p) != 0u;
    out->filter_ms                 = get_u16(&p);
    out->fault_state               = (fault_state_t)get_u8(&p);
    out->fault_value_c10           = get_i16(&p);
    return TLV_OK;
}

/* --------------------------------------------------------------------- */
/* AI                                                                    */
/* --------------------------------------------------------------------- */

tlv_status_t
config_codec_encode_ai (const ai_config_t * in,
                        void *              buf,
                        size_t              cap,
                        size_t *            out_size)
{
    tlv_status_t st = check_encode(in, buf, cap, CONFIG_CODEC_AI_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    uint8_t * p = (uint8_t *)buf;
    put_bytes(&p, in->name, CONFIG_NAME_LEN);
    put_u16(&p, in->id);
    put_u8(&p, (uint8_t)in->input_mode);
    put_u16(&p, in->filter_ms);
    put_i32(&p, in->scale_num);
    put_i32(&p, in->scale_den);
    put_i32(&p, in->offset);
    put_u8(&p, (uint8_t)in->fault_state);
    put_i32(&p, in->fault_value);
    assert((size_t)(p - (uint8_t *)buf) == CONFIG_CODEC_AI_WIRE_SIZE);
    *out_size = CONFIG_CODEC_AI_WIRE_SIZE;
    return TLV_OK;
}

tlv_status_t
config_codec_decode_ai (const void * buf, size_t len, ai_config_t * out)
{
    tlv_status_t st = check_decode(buf, out, len, CONFIG_CODEC_AI_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    const uint8_t * p = (const uint8_t *)buf;
    get_bytes(&p, out->name, CONFIG_NAME_LEN);
    out->name[CONFIG_NAME_LEN - 1] = '\0';
    out->id                        = get_u16(&p);
    out->input_mode                = (ai_input_mode_t)get_u8(&p);
    out->filter_ms                 = get_u16(&p);
    out->scale_num                 = get_i32(&p);
    out->scale_den                 = get_i32(&p);
    out->offset                    = get_i32(&p);
    out->fault_state               = (fault_state_t)get_u8(&p);
    out->fault_value               = get_i32(&p);
    return TLV_OK;
}

/* --------------------------------------------------------------------- */
/* AO                                                                    */
/* --------------------------------------------------------------------- */

tlv_status_t
config_codec_encode_ao (const ao_config_t * in,
                        void *              buf,
                        size_t              cap,
                        size_t *            out_size)
{
    tlv_status_t st = check_encode(in, buf, cap, CONFIG_CODEC_AO_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    uint8_t * p = (uint8_t *)buf;
    put_bytes(&p, in->name, CONFIG_NAME_LEN);
    put_u16(&p, in->id);
    put_u8(&p, (uint8_t)in->output_mode);
    put_u16(&p, in->slew_per_s);
    put_i32(&p, in->scale_num);
    put_i32(&p, in->scale_den);
    put_i32(&p, in->offset);
    put_u8(&p, (uint8_t)in->fault_state);
    put_i32(&p, in->fault_value);
    assert((size_t)(p - (uint8_t *)buf) == CONFIG_CODEC_AO_WIRE_SIZE);
    *out_size = CONFIG_CODEC_AO_WIRE_SIZE;
    return TLV_OK;
}

tlv_status_t
config_codec_decode_ao (const void * buf, size_t len, ao_config_t * out)
{
    tlv_status_t st = check_decode(buf, out, len, CONFIG_CODEC_AO_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    const uint8_t * p = (const uint8_t *)buf;
    get_bytes(&p, out->name, CONFIG_NAME_LEN);
    out->name[CONFIG_NAME_LEN - 1] = '\0';
    out->id                        = get_u16(&p);
    out->output_mode               = (ao_output_mode_t)get_u8(&p);
    out->slew_per_s                = get_u16(&p);
    out->scale_num                 = get_i32(&p);
    out->scale_den                 = get_i32(&p);
    out->offset                    = get_i32(&p);
    out->fault_state               = (fault_state_t)get_u8(&p);
    out->fault_value               = get_i32(&p);
    return TLV_OK;
}

/* --------------------------------------------------------------------- */
/* PCNT                                                                  */
/* --------------------------------------------------------------------- */

tlv_status_t
config_codec_encode_pcnt (const pcnt_config_t * in,
                          void *                buf,
                          size_t                cap,
                          size_t *              out_size)
{
    tlv_status_t st = check_encode(in, buf, cap, CONFIG_CODEC_PCNT_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    uint8_t * p = (uint8_t *)buf;
    put_bytes(&p, in->name, CONFIG_NAME_LEN);
    put_u16(&p, in->id);
    put_u8(&p, (uint8_t)in->mode);
    put_u8(&p, (uint8_t)in->edge);
    put_u32(&p, in->limit);
    put_u8(&p, in->reset_on_read ? 1u : 0u);
    assert((size_t)(p - (uint8_t *)buf) == CONFIG_CODEC_PCNT_WIRE_SIZE);
    *out_size = CONFIG_CODEC_PCNT_WIRE_SIZE;
    return TLV_OK;
}

tlv_status_t
config_codec_decode_pcnt (const void * buf, size_t len, pcnt_config_t * out)
{
    tlv_status_t st = check_decode(buf, out, len, CONFIG_CODEC_PCNT_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    const uint8_t * p = (const uint8_t *)buf;
    get_bytes(&p, out->name, CONFIG_NAME_LEN);
    out->name[CONFIG_NAME_LEN - 1] = '\0';
    out->id                        = get_u16(&p);
    out->mode                      = (pcnt_mode_t)get_u8(&p);
    out->edge                      = (pcnt_edge_t)get_u8(&p);
    out->limit                     = get_u32(&p);
    out->reset_on_read             = get_u8(&p) != 0u;
    return TLV_OK;
}

/* --------------------------------------------------------------------- */
/* PWM                                                                   */
/* --------------------------------------------------------------------- */

tlv_status_t
config_codec_encode_pwm (const pwm_config_t * in,
                         void *               buf,
                         size_t               cap,
                         size_t *             out_size)
{
    tlv_status_t st = check_encode(in, buf, cap, CONFIG_CODEC_PWM_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    uint8_t * p = (uint8_t *)buf;
    put_bytes(&p, in->name, CONFIG_NAME_LEN);
    put_u16(&p, in->id);
    put_u32(&p, in->period_us);
    put_u16(&p, in->duty_permille);
    put_u8(&p, (uint8_t)in->fault_state);
    put_u16(&p, in->fault_duty_permille);
    assert((size_t)(p - (uint8_t *)buf) == CONFIG_CODEC_PWM_WIRE_SIZE);
    *out_size = CONFIG_CODEC_PWM_WIRE_SIZE;
    return TLV_OK;
}

tlv_status_t
config_codec_decode_pwm (const void * buf, size_t len, pwm_config_t * out)
{
    tlv_status_t st = check_decode(buf, out, len, CONFIG_CODEC_PWM_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    const uint8_t * p = (const uint8_t *)buf;
    get_bytes(&p, out->name, CONFIG_NAME_LEN);
    out->name[CONFIG_NAME_LEN - 1] = '\0';
    out->id                        = get_u16(&p);
    out->period_us                 = get_u32(&p);
    out->duty_permille             = get_u16(&p);
    out->fault_state               = (fault_state_t)get_u8(&p);
    out->fault_duty_permille       = get_u16(&p);
    return TLV_OK;
}

/* --------------------------------------------------------------------- */
/* System                                                                */
/* --------------------------------------------------------------------- */

tlv_status_t
config_codec_encode_system (const system_config_t * in,
                            void *                  buf,
                            size_t                  cap,
                            size_t *                out_size)
{
    tlv_status_t st = check_encode(in, buf, cap, CONFIG_CODEC_SYSTEM_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    uint8_t * p = (uint8_t *)buf;
    put_u8(&p, in->canopen_node_id);
    put_u8(&p, (uint8_t)in->can_bitrate);
    put_u16(&p, in->heartbeat_ms);
    put_u16(&p, in->sync_window_us);
    put_u8(&p, (uint8_t)in->nmt_startup);
    put_u16(&p, in->producer_emcy_cob_id);
    assert((size_t)(p - (uint8_t *)buf) == CONFIG_CODEC_SYSTEM_WIRE_SIZE);
    *out_size = CONFIG_CODEC_SYSTEM_WIRE_SIZE;
    return TLV_OK;
}

tlv_status_t
config_codec_decode_system (const void * buf, size_t len, system_config_t * out)
{
    tlv_status_t st
        = check_decode(buf, out, len, CONFIG_CODEC_SYSTEM_WIRE_SIZE);
    if (st != TLV_OK)
    {
        return st;
    }
    const uint8_t * p         = (const uint8_t *)buf;
    out->canopen_node_id      = get_u8(&p);
    out->can_bitrate          = (can_bitrate_t)get_u8(&p);
    out->heartbeat_ms         = get_u16(&p);
    out->sync_window_us       = get_u16(&p);
    out->nmt_startup          = (nmt_startup_t)get_u8(&p);
    out->producer_emcy_cob_id = get_u16(&p);
    return TLV_OK;
}
