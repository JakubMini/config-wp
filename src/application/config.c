/*****************************************************************************
 * Module:  config
 * Purpose: Implementation of the manager API declared in config.h.
 *
 *          Pieces:
 *            - s_cache:          POD struct holding every record + system
 *            - s_unknown[]:      module-static buffer holding the raw bytes
 *                                of TLV records whose tags the manager
 *                                didn't recognise during config_init. These
 *                                are emitted verbatim on config_save so
 *                                older firmware doesn't silently destroy
 *                                newer-firmware configuration.
 *            - s_blob_buf[]:     module-static buffer used by config_save
 *                                to encode the whole cache before handing
 *                                bytes to slot_write. Sized to the slot's
 *                                max payload.
 *            - config_lock:      the single mutex protecting s_cache and
 *                                s_unknown. Held for the duration of every
 *                                getter/setter; released before slot_write
 *                                during config_save.
 *
 *          The "encode-under-lock, write-outside-lock" pattern in
 *          config_save keeps the lock-hold time bounded by an in-RAM
 *          encode (microseconds) rather than an SPI transaction
 *          (milliseconds), which would otherwise stall the high-priority
 *          IO thread that polls the cache.
 *****************************************************************************/

#include "application/config.h"

#include "application/config_codec.h"
#include "application/config_defaults.h"
#include "application/config_limits.h"
#include "application/config_lock.h"
#include "application/config_slot.h"
#include "application/config_types.h"
#include "application/crc32.h"
#include "application/tlv.h"
#include "drivers/storage.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ========================================================================
 * Module state
 * ======================================================================== */

typedef struct
{
    di_config_t     di[CONFIG_NUM_DI];
    do_config_t     do_[CONFIG_NUM_DO];
    tc_config_t     tc[CONFIG_NUM_TC];
    ai_config_t     ai[CONFIG_NUM_AI];
    ao_config_t     ao[CONFIG_NUM_AO];
    pcnt_config_t   pcnt[CONFIG_NUM_PCNT];
    pwm_config_t    pwm[CONFIG_NUM_PWM];
    system_config_t system;
} config_cache_t;

/* Headroom for TLV records the manager didn't recognise during init.
 * 1 KB is plenty for one or two future-firmware record types to slip
 * through and round-trip. */
#define UNKNOWN_BUF_BYTES 1024u

static config_cache_t s_cache;
static uint8_t        s_unknown[UNKNOWN_BUF_BYTES];
static size_t         s_unknown_used = 0;
static uint8_t        s_blob_buf[SLOT_PAYLOAD_MAX_BYTES];
static bool           s_initialised = false;

/* ========================================================================
 * Defaults loading (callers hold the lock)
 * ======================================================================== */

static void
load_defaults_locked (void)
{
    memcpy(s_cache.di, g_di_defaults, sizeof(s_cache.di));
    memcpy(s_cache.do_, g_do_defaults, sizeof(s_cache.do_));
    memcpy(s_cache.tc, g_tc_defaults, sizeof(s_cache.tc));
    memcpy(s_cache.ai, g_ai_defaults, sizeof(s_cache.ai));
    memcpy(s_cache.ao, g_ao_defaults, sizeof(s_cache.ao));
    memcpy(s_cache.pcnt, g_pcnt_defaults, sizeof(s_cache.pcnt));
    memcpy(s_cache.pwm, g_pwm_defaults, sizeof(s_cache.pwm));
    s_cache.system = g_system_defaults;
    s_unknown_used = 0;
}

/* ========================================================================
 * Validators (called BEFORE taking the lock — cheap early reject)
 * ======================================================================== */

static bool
name_is_terminated (const char * name)
{
    assert(name != NULL);
    return name[CONFIG_NAME_LEN - 1] == '\0';
}

static bool
validate_di (const di_config_t * in)
{
    if (!name_is_terminated(in->name))
    {
        return false;
    }
    if ((unsigned)in->polarity >= (unsigned)DI_POLARITY_COUNT)
    {
        return false;
    }
    if ((unsigned)in->fault_state >= (unsigned)FAULT_STATE_COUNT)
    {
        return false;
    }
    return true;
}

static bool
validate_do (const do_config_t * in)
{
    if (!name_is_terminated(in->name))
    {
        return false;
    }
    if ((unsigned)in->polarity >= (unsigned)DO_POLARITY_COUNT)
    {
        return false;
    }
    if ((unsigned)in->fault_state >= (unsigned)FAULT_STATE_COUNT)
    {
        return false;
    }
    return true;
}

static bool
validate_tc (const tc_config_t * in)
{
    if (!name_is_terminated(in->name))
    {
        return false;
    }
    if ((unsigned)in->tc_type >= (unsigned)TC_TYPE_COUNT)
    {
        return false;
    }
    if ((unsigned)in->unit >= (unsigned)TC_UNIT_COUNT)
    {
        return false;
    }
    if ((unsigned)in->fault_state >= (unsigned)FAULT_STATE_COUNT)
    {
        return false;
    }
    return true;
}

static bool
validate_ai (const ai_config_t * in)
{
    if (!name_is_terminated(in->name))
    {
        return false;
    }
    if ((unsigned)in->input_mode >= (unsigned)AI_INPUT_MODE_COUNT)
    {
        return false;
    }
    if ((unsigned)in->fault_state >= (unsigned)FAULT_STATE_COUNT)
    {
        return false;
    }
    if (in->scale_den == 0)
    {
        return false;
    }
    return true;
}

static bool
validate_ao (const ao_config_t * in)
{
    if (!name_is_terminated(in->name))
    {
        return false;
    }
    if ((unsigned)in->output_mode >= (unsigned)AO_OUTPUT_MODE_COUNT)
    {
        return false;
    }
    if ((unsigned)in->fault_state >= (unsigned)FAULT_STATE_COUNT)
    {
        return false;
    }
    if (in->scale_den == 0)
    {
        return false;
    }
    return true;
}

static bool
validate_pcnt (const pcnt_config_t * in)
{
    if (!name_is_terminated(in->name))
    {
        return false;
    }
    if ((unsigned)in->mode >= (unsigned)PCNT_MODE_COUNT)
    {
        return false;
    }
    if ((unsigned)in->edge >= (unsigned)PCNT_EDGE_COUNT)
    {
        return false;
    }
    return true;
}

static bool
validate_pwm (const pwm_config_t * in)
{
    if (!name_is_terminated(in->name))
    {
        return false;
    }
    if (in->period_us == 0)
    {
        return false;
    }
    if (in->duty_permille > 1000)
    {
        return false;
    }
    if (in->fault_duty_permille > 1000)
    {
        return false;
    }
    if ((unsigned)in->fault_state >= (unsigned)FAULT_STATE_COUNT)
    {
        return false;
    }
    return true;
}

static bool
validate_system (const system_config_t * in)
{
    if (in->canopen_node_id < 1 || in->canopen_node_id > 127)
    {
        return false;
    }
    if ((unsigned)in->can_bitrate >= (unsigned)CAN_BITRATE_COUNT)
    {
        return false;
    }
    if ((unsigned)in->nmt_startup >= (unsigned)NMT_STARTUP_COUNT)
    {
        return false;
    }
    /* producer_emcy_cob_id: 0 (sentinel, derive at NMT startup) OR
     * >= 0x81 (operator override; 0x80 is the SYNC COB-ID and would
     * clash). */
    if (in->producer_emcy_cob_id != 0 && in->producer_emcy_cob_id < 0x81u)
    {
        return false;
    }
    return true;
}

/* ========================================================================
 * Unknown TLV preservation
 * ======================================================================== */

/* Stash a raw TLV record (header + value) in s_unknown. Called from
 * decode_blob_locked when a tag isn't recognised. Silently drops the
 * record if there's no room — better than overwriting other unknowns
 * or refusing to load the rest of the config. */
static void
preserve_unknown_locked (uint16_t tag, const void * value, size_t value_len)
{
    const size_t need = (size_t)TLV_HEADER_BYTES + value_len;
    if (s_unknown_used + need > UNKNOWN_BUF_BYTES)
    {
        return;
    }
    tlv_writer_t w;
    tlv_writer_init(
        &w, &s_unknown[s_unknown_used], UNKNOWN_BUF_BYTES - s_unknown_used);
    if (tlv_writer_emit(&w, tag, value, value_len) == TLV_OK)
    {
        s_unknown_used += tlv_writer_size(&w);
    }
}

/* ========================================================================
 * Decoding a blob into the cache (caller holds the lock)
 * ======================================================================== */

static config_status_t
decode_into_record_locked (uint16_t tag, const void * value, size_t value_len)
{
    const uint8_t domain = config_codec_tag_domain(tag);
    const uint8_t index  = config_codec_tag_index(tag);
    tlv_status_t  ts     = TLV_OK;

    switch (domain)
    {
        case CONFIG_CODEC_DOMAIN_DI:
            if (index < CONFIG_NUM_DI)
            {
                ts = config_codec_decode_di(
                    value, value_len, &s_cache.di[index]);
            }
            else
            {
                preserve_unknown_locked(tag, value, value_len);
            }
            break;
        case CONFIG_CODEC_DOMAIN_DO:
            if (index < CONFIG_NUM_DO)
            {
                ts = config_codec_decode_do(
                    value, value_len, &s_cache.do_[index]);
            }
            else
            {
                preserve_unknown_locked(tag, value, value_len);
            }
            break;
        case CONFIG_CODEC_DOMAIN_TC:
            if (index < CONFIG_NUM_TC)
            {
                ts = config_codec_decode_tc(
                    value, value_len, &s_cache.tc[index]);
            }
            else
            {
                preserve_unknown_locked(tag, value, value_len);
            }
            break;
        case CONFIG_CODEC_DOMAIN_AI:
            if (index < CONFIG_NUM_AI)
            {
                ts = config_codec_decode_ai(
                    value, value_len, &s_cache.ai[index]);
            }
            else
            {
                preserve_unknown_locked(tag, value, value_len);
            }
            break;
        case CONFIG_CODEC_DOMAIN_AO:
            if (index < CONFIG_NUM_AO)
            {
                ts = config_codec_decode_ao(
                    value, value_len, &s_cache.ao[index]);
            }
            else
            {
                preserve_unknown_locked(tag, value, value_len);
            }
            break;
        case CONFIG_CODEC_DOMAIN_PCNT:
            if (index < CONFIG_NUM_PCNT)
            {
                ts = config_codec_decode_pcnt(
                    value, value_len, &s_cache.pcnt[index]);
            }
            else
            {
                preserve_unknown_locked(tag, value, value_len);
            }
            break;
        case CONFIG_CODEC_DOMAIN_PWM:
            if (index < CONFIG_NUM_PWM)
            {
                ts = config_codec_decode_pwm(
                    value, value_len, &s_cache.pwm[index]);
            }
            else
            {
                preserve_unknown_locked(tag, value, value_len);
            }
            break;
        case CONFIG_CODEC_DOMAIN_SYSTEM:
            ts = config_codec_decode_system(value, value_len, &s_cache.system);
            break;
        default:
            preserve_unknown_locked(tag, value, value_len);
            break;
    }
    return (ts == TLV_OK) ? CONFIG_OK : CONFIG_ERR_CODEC;
}

static config_status_t
decode_blob_locked (const uint8_t * blob, size_t blob_len)
{
    tlv_iter_t it;
    tlv_iter_init(&it, blob, blob_len);

    for (;;)
    {
        uint16_t     tag       = 0;
        const void * value     = NULL;
        size_t       value_len = 0;
        tlv_status_t ts        = tlv_iter_next(&it, &tag, &value, &value_len);
        if (ts == TLV_END)
        {
            return CONFIG_OK;
        }
        if (ts != TLV_OK)
        {
            return CONFIG_ERR_CODEC;
        }
        const config_status_t cs
            = decode_into_record_locked(tag, value, value_len);
        if (cs != CONFIG_OK)
        {
            return cs;
        }
    }
}

/* ========================================================================
 * Encoding the cache into a blob (caller holds the lock)
 * ======================================================================== */

typedef tlv_status_t (*encode_value_fn)(const void * in,
                                        void *       buf,
                                        size_t       cap,
                                        size_t *     out_size);

/* Encode one record's value bytes and wrap with a TLV header. */
static config_status_t
emit_record_locked (tlv_writer_t *  w,
                    uint8_t         domain,
                    uint8_t         index,
                    encode_value_fn enc,
                    const void *    record)
{
    uint8_t      value_buf[64]; /* max wire size today is 38 (AI/AO) */
    size_t       value_size = 0;
    tlv_status_t ts = enc(record, value_buf, sizeof(value_buf), &value_size);
    if (ts != TLV_OK)
    {
        return CONFIG_ERR_CODEC;
    }
    const uint16_t tag = config_codec_make_tag(domain, index);
    ts                 = tlv_writer_emit(w, tag, value_buf, value_size);
    if (ts == TLV_ERR_NO_SPACE)
    {
        return CONFIG_ERR_TOO_LARGE;
    }
    if (ts != TLV_OK)
    {
        return CONFIG_ERR_CODEC;
    }
    return CONFIG_OK;
}

static config_status_t
encode_cache_locked (uint8_t * buf, size_t cap, size_t * out_size)
{
    tlv_writer_t w;
    tlv_writer_init(&w, buf, cap);

    for (uint8_t i = 0; i < CONFIG_NUM_DI; ++i)
    {
        const config_status_t cs
            = emit_record_locked(&w,
                                 CONFIG_CODEC_DOMAIN_DI,
                                 i,
                                 (encode_value_fn)config_codec_encode_di,
                                 &s_cache.di[i]);
        if (cs != CONFIG_OK)
        {
            return cs;
        }
    }
    for (uint8_t i = 0; i < CONFIG_NUM_DO; ++i)
    {
        const config_status_t cs
            = emit_record_locked(&w,
                                 CONFIG_CODEC_DOMAIN_DO,
                                 i,
                                 (encode_value_fn)config_codec_encode_do,
                                 &s_cache.do_[i]);
        if (cs != CONFIG_OK)
        {
            return cs;
        }
    }
    for (uint8_t i = 0; i < CONFIG_NUM_TC; ++i)
    {
        const config_status_t cs
            = emit_record_locked(&w,
                                 CONFIG_CODEC_DOMAIN_TC,
                                 i,
                                 (encode_value_fn)config_codec_encode_tc,
                                 &s_cache.tc[i]);
        if (cs != CONFIG_OK)
        {
            return cs;
        }
    }
    for (uint8_t i = 0; i < CONFIG_NUM_AI; ++i)
    {
        const config_status_t cs
            = emit_record_locked(&w,
                                 CONFIG_CODEC_DOMAIN_AI,
                                 i,
                                 (encode_value_fn)config_codec_encode_ai,
                                 &s_cache.ai[i]);
        if (cs != CONFIG_OK)
        {
            return cs;
        }
    }
    for (uint8_t i = 0; i < CONFIG_NUM_AO; ++i)
    {
        const config_status_t cs
            = emit_record_locked(&w,
                                 CONFIG_CODEC_DOMAIN_AO,
                                 i,
                                 (encode_value_fn)config_codec_encode_ao,
                                 &s_cache.ao[i]);
        if (cs != CONFIG_OK)
        {
            return cs;
        }
    }
    for (uint8_t i = 0; i < CONFIG_NUM_PCNT; ++i)
    {
        const config_status_t cs
            = emit_record_locked(&w,
                                 CONFIG_CODEC_DOMAIN_PCNT,
                                 i,
                                 (encode_value_fn)config_codec_encode_pcnt,
                                 &s_cache.pcnt[i]);
        if (cs != CONFIG_OK)
        {
            return cs;
        }
    }
    for (uint8_t i = 0; i < CONFIG_NUM_PWM; ++i)
    {
        const config_status_t cs
            = emit_record_locked(&w,
                                 CONFIG_CODEC_DOMAIN_PWM,
                                 i,
                                 (encode_value_fn)config_codec_encode_pwm,
                                 &s_cache.pwm[i]);
        if (cs != CONFIG_OK)
        {
            return cs;
        }
    }

    const config_status_t cs
        = emit_record_locked(&w,
                             CONFIG_CODEC_DOMAIN_SYSTEM,
                             CONFIG_CODEC_SYSTEM_INDEX,
                             (encode_value_fn)config_codec_encode_system,
                             &s_cache.system);
    if (cs != CONFIG_OK)
    {
        return cs;
    }

    /* Append preserved unknown TLV records verbatim. s_unknown holds
     * pre-encoded TLV records; walk it as bytes and re-emit each one. */
    size_t cursor = 0;
    while (cursor < s_unknown_used)
    {
        if (cursor + (size_t)TLV_HEADER_BYTES > s_unknown_used)
        {
            break;
        }
        const uint16_t ulen = (uint16_t)s_unknown[cursor + 2u]
                              | ((uint16_t)s_unknown[cursor + 3u] << 8);
        const size_t total = (size_t)TLV_HEADER_BYTES + (size_t)ulen;
        if (cursor + total > s_unknown_used)
        {
            break;
        }
        const tlv_status_t ts
            = tlv_writer_emit_raw(&w, &s_unknown[cursor], total);
        if (ts == TLV_ERR_NO_SPACE)
        {
            return CONFIG_ERR_TOO_LARGE;
        }
        if (ts != TLV_OK)
        {
            return CONFIG_ERR_CODEC;
        }
        cursor += total;
    }

    *out_size = tlv_writer_size(&w);
    return CONFIG_OK;
}

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

config_status_t
config_init (void)
{
    if (s_initialised)
    {
        return CONFIG_OK;
    }

    crc32_init();
    if (!config_lock_create())
    {
        /* Mutex creation failed — distinct from "API called before init"
         * (CONFIG_ERR_NOT_INITIALISED) so callers can tell them apart. */
        return CONFIG_ERR_INTERNAL;
    }

    /* Defaults + slot decode happen while s_initialised is still false.
     * That keeps every public getter/setter rejected with
     * CONFIG_ERR_NOT_INITIALISED until init finishes — so no other task
     * can observe a half-loaded cache, and decode_blob can't be raced by
     * a concurrent setter. The lock is still taken for the brief windows
     * we mutate the cache so this module's own internal helpers stay
     * concurrency-safe. Caller contract: config_init is called from one
     * thread at startup. */
    config_lock_take();
    load_defaults_locked();
    config_lock_give();

    slot_id_t     id  = SLOT_NONE;
    size_t        len = 0;
    slot_status_t st
        = slot_pick_active(&id, s_blob_buf, sizeof(s_blob_buf), &len);
    (void)id;

    config_status_t result = CONFIG_OK;
    if (st == SLOT_OK)
    {
        config_lock_take();
        result = decode_blob_locked(s_blob_buf, len);
        config_lock_give();
    }
    else if (st == SLOT_ERR_NO_VALID)
    {
        result = CONFIG_OK; /* blank EEPROM — defaults stand */
    }
    else
    {
        result = CONFIG_ERR_STORAGE;
    }

    /* Cache is now fully populated (defaults + any overlay from the
     * loaded slot). Allow public API access. */
    s_initialised = true;
    return result;
}

void
config_deinit (void)
{
    if (!s_initialised)
    {
        return;
    }
    config_lock_destroy();
    s_initialised  = false;
    s_unknown_used = 0;
}

config_status_t
config_reset_defaults (void)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    config_lock_take();
    load_defaults_locked();
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_save (void)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }

    size_t blob_len = 0;
    config_lock_take();
    const config_status_t cs
        = encode_cache_locked(s_blob_buf, sizeof(s_blob_buf), &blob_len);
    config_lock_give();
    if (cs != CONFIG_OK)
    {
        return cs;
    }

    /* slot_write runs OUTSIDE the lock. Caller contract: config_save
     * is called from a single thread (the EEPROM Manager in the
     * firmware architecture). s_blob_buf is owned by the in-flight
     * save until slot_write returns.
     *
     * Priority-inversion guard: a refactor that left the cache mutex
     * held across this storage I/O would stall the high-priority IO
     * task for the duration of the SPI transaction (ms+). Assert that
     * we don't hold it before crossing the boundary. */
    assert(!config_lock_is_held_by_current_thread()
           && "config_lock must be released before storage I/O");
    const slot_status_t ss = slot_write(s_blob_buf, blob_len);
    return (ss == SLOT_OK) ? CONFIG_OK : CONFIG_ERR_STORAGE;
}

/* ========================================================================
 * Per-type getters and setters
 *
 * Every function follows the same shape:
 *   1. initialisation check
 *   2. NULL pointer check
 *   3. index bounds check
 *   4. (setters only) validate inputs
 *   5. take lock -> copy -> give lock
 *   6. return status
 * ======================================================================== */

config_status_t
config_get_di (uint8_t idx, di_config_t * out)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (out == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_DI)
    {
        return CONFIG_ERR_INDEX;
    }
    config_lock_take();
    *out = s_cache.di[idx];
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_set_di (uint8_t idx, const di_config_t * in)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (in == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_DI)
    {
        return CONFIG_ERR_INDEX;
    }
    if (!validate_di(in))
    {
        return CONFIG_ERR_INVALID;
    }
    config_lock_take();
    s_cache.di[idx] = *in;
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_get_do (uint8_t idx, do_config_t * out)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (out == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_DO)
    {
        return CONFIG_ERR_INDEX;
    }
    config_lock_take();
    *out = s_cache.do_[idx];
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_set_do (uint8_t idx, const do_config_t * in)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (in == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_DO)
    {
        return CONFIG_ERR_INDEX;
    }
    if (!validate_do(in))
    {
        return CONFIG_ERR_INVALID;
    }
    config_lock_take();
    s_cache.do_[idx] = *in;
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_get_tc (uint8_t idx, tc_config_t * out)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (out == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_TC)
    {
        return CONFIG_ERR_INDEX;
    }
    config_lock_take();
    *out = s_cache.tc[idx];
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_set_tc (uint8_t idx, const tc_config_t * in)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (in == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_TC)
    {
        return CONFIG_ERR_INDEX;
    }
    if (!validate_tc(in))
    {
        return CONFIG_ERR_INVALID;
    }
    config_lock_take();
    s_cache.tc[idx] = *in;
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_get_ai (uint8_t idx, ai_config_t * out)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (out == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_AI)
    {
        return CONFIG_ERR_INDEX;
    }
    config_lock_take();
    *out = s_cache.ai[idx];
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_set_ai (uint8_t idx, const ai_config_t * in)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (in == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_AI)
    {
        return CONFIG_ERR_INDEX;
    }
    if (!validate_ai(in))
    {
        return CONFIG_ERR_INVALID;
    }
    config_lock_take();
    s_cache.ai[idx] = *in;
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_get_ao (uint8_t idx, ao_config_t * out)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (out == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_AO)
    {
        return CONFIG_ERR_INDEX;
    }
    config_lock_take();
    *out = s_cache.ao[idx];
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_set_ao (uint8_t idx, const ao_config_t * in)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (in == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_AO)
    {
        return CONFIG_ERR_INDEX;
    }
    if (!validate_ao(in))
    {
        return CONFIG_ERR_INVALID;
    }
    config_lock_take();
    s_cache.ao[idx] = *in;
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_get_pcnt (uint8_t idx, pcnt_config_t * out)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (out == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_PCNT)
    {
        return CONFIG_ERR_INDEX;
    }
    config_lock_take();
    *out = s_cache.pcnt[idx];
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_set_pcnt (uint8_t idx, const pcnt_config_t * in)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (in == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_PCNT)
    {
        return CONFIG_ERR_INDEX;
    }
    if (!validate_pcnt(in))
    {
        return CONFIG_ERR_INVALID;
    }
    config_lock_take();
    s_cache.pcnt[idx] = *in;
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_get_pwm (uint8_t idx, pwm_config_t * out)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (out == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_PWM)
    {
        return CONFIG_ERR_INDEX;
    }
    config_lock_take();
    *out = s_cache.pwm[idx];
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_set_pwm (uint8_t idx, const pwm_config_t * in)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (in == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (idx >= CONFIG_NUM_PWM)
    {
        return CONFIG_ERR_INDEX;
    }
    if (!validate_pwm(in))
    {
        return CONFIG_ERR_INVALID;
    }
    config_lock_take();
    s_cache.pwm[idx] = *in;
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_get_system (system_config_t * out)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (out == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    config_lock_take();
    *out = s_cache.system;
    config_lock_give();
    return CONFIG_OK;
}

config_status_t
config_set_system (const system_config_t * in)
{
    if (!s_initialised)
    {
        return CONFIG_ERR_NOT_INITIALISED;
    }
    if (in == NULL)
    {
        return CONFIG_ERR_INVALID;
    }
    if (!validate_system(in))
    {
        return CONFIG_ERR_INVALID;
    }
    config_lock_take();
    s_cache.system = *in;
    config_lock_give();
    return CONFIG_OK;
}
