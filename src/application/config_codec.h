/*****************************************************************************
 * Module:  config_codec
 * Purpose: Type-aware wrappers over the generic TLV codec. Provides:
 *
 *            - Tag conventions: how (domain, index) packs into a u16 tag.
 *            - Wire-format encoders / decoders for every IO struct and
 *              the system_config struct.
 *            - Compile-time wire-size constants so callers can size
 *              buffers and tests can pin the on-flash layout.
 *
 *          Wire layout is little-endian, field-by-field, with NO padding.
 *          Decoupling the wire format from in-RAM struct padding means a
 *          config written on one toolchain is readable by another, and a
 *          struct field addition doesn't silently shift offsets in
 *          existing on-flash records.
 *
 *          Each per-type encoder writes the value bytes only — the TLV
 *          header (tag + length) is the caller's job to wrap via
 *          tlv_writer_emit(). This split lets the caller stream records
 *          into a single buffer without an extra copy.
 *****************************************************************************/

#ifndef APPLICATION_CONFIG_CODEC_H
#define APPLICATION_CONFIG_CODEC_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "application/config_types.h"
#include "application/tlv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------- */
/* Tag conventions                                                       */
/* --------------------------------------------------------------------- */
/*
 * Tag layout (16 bits):
 *   bits 15..8  domain     identifies the record type
 *   bits 7..0   index      channel number within the domain
 *
 * IO domain values match io_domain_t so the two enums stay in lockstep
 * — adding a new IO type adds one io_domain_t value and one tag space.
 * SYSTEM uses a high reserved value (0xF0) to keep it clearly distinct.
 */
#define CONFIG_CODEC_DOMAIN_DI     ((uint8_t)IO_DOMAIN_DI)
#define CONFIG_CODEC_DOMAIN_DO     ((uint8_t)IO_DOMAIN_DO)
#define CONFIG_CODEC_DOMAIN_TC     ((uint8_t)IO_DOMAIN_TC)
#define CONFIG_CODEC_DOMAIN_AI     ((uint8_t)IO_DOMAIN_AI)
#define CONFIG_CODEC_DOMAIN_AO     ((uint8_t)IO_DOMAIN_AO)
#define CONFIG_CODEC_DOMAIN_PCNT   ((uint8_t)IO_DOMAIN_PCNT)
#define CONFIG_CODEC_DOMAIN_PWM    ((uint8_t)IO_DOMAIN_PWM)
#define CONFIG_CODEC_DOMAIN_SYSTEM ((uint8_t)0xF0u)

/* The system record has no per-channel index; pinned to 0. */
#define CONFIG_CODEC_SYSTEM_INDEX 0u

static inline uint16_t
config_codec_make_tag (uint8_t domain, uint8_t index)
{
    return (uint16_t)(((uint16_t)domain << 8) | (uint16_t)index);
}

static inline uint8_t
config_codec_tag_domain (uint16_t tag)
{
    return (uint8_t)((tag >> 8) & 0xFFu);
}

static inline uint8_t
config_codec_tag_index (uint16_t tag)
{
    return (uint8_t)(tag & 0xFFu);
}

/* --------------------------------------------------------------------- */
/* Wire sizes (constant, NOT struct sizeof)                              */
/* --------------------------------------------------------------------- */
/* Each constant is the byte count of the *value* portion of a TLV
 * record for that type — exclusive of the 4-byte TLV header. Pinned by
 * unit tests so they're a stable on-flash contract. */
#define CONFIG_CODEC_DI_WIRE_SIZE     23u
#define CONFIG_CODEC_DO_WIRE_SIZE     20u
#define CONFIG_CODEC_TC_WIRE_SIZE     26u
#define CONFIG_CODEC_AI_WIRE_SIZE     38u
#define CONFIG_CODEC_AO_WIRE_SIZE     38u
#define CONFIG_CODEC_PCNT_WIRE_SIZE   25u
#define CONFIG_CODEC_PWM_WIRE_SIZE    27u
#define CONFIG_CODEC_SYSTEM_WIRE_SIZE 9u

/* --------------------------------------------------------------------- */
/* Per-type encoders / decoders                                          */
/* --------------------------------------------------------------------- */
/*
 * Each encode_* serialises the struct into `buf` (writing exactly the
 * wire-size bytes) and reports the size via *out_size on success.
 *
 * Each decode_* reads exactly the wire-size bytes from `buf` and
 * populates `out`. A buf_len shorter than the expected wire size
 * returns TLV_ERR_TRUNCATED. A longer buf_len is permitted — trailing
 * bytes within the same record are ignored, which lets older firmware
 * read newer records that grew fields (the field-level forward-compat
 * limitation is documented in DESIGN.md).
 */
tlv_status_t config_codec_encode_di (const di_config_t * in,
                                     void *              buf,
                                     size_t              cap,
                                     size_t *            out_size);
tlv_status_t config_codec_decode_di (const void *  buf,
                                     size_t        len,
                                     di_config_t * out);

tlv_status_t config_codec_encode_do (const do_config_t * in,
                                     void *              buf,
                                     size_t              cap,
                                     size_t *            out_size);
tlv_status_t config_codec_decode_do (const void *  buf,
                                     size_t        len,
                                     do_config_t * out);

tlv_status_t config_codec_encode_tc (const tc_config_t * in,
                                     void *              buf,
                                     size_t              cap,
                                     size_t *            out_size);
tlv_status_t config_codec_decode_tc (const void *  buf,
                                     size_t        len,
                                     tc_config_t * out);

tlv_status_t config_codec_encode_ai (const ai_config_t * in,
                                     void *              buf,
                                     size_t              cap,
                                     size_t *            out_size);
tlv_status_t config_codec_decode_ai (const void *  buf,
                                     size_t        len,
                                     ai_config_t * out);

tlv_status_t config_codec_encode_ao (const ao_config_t * in,
                                     void *              buf,
                                     size_t              cap,
                                     size_t *            out_size);
tlv_status_t config_codec_decode_ao (const void *  buf,
                                     size_t        len,
                                     ao_config_t * out);

tlv_status_t config_codec_encode_pcnt (const pcnt_config_t * in,
                                       void *                buf,
                                       size_t                cap,
                                       size_t *              out_size);
tlv_status_t config_codec_decode_pcnt (const void *    buf,
                                       size_t          len,
                                       pcnt_config_t * out);

tlv_status_t config_codec_encode_pwm (const pwm_config_t * in,
                                      void *               buf,
                                      size_t               cap,
                                      size_t *             out_size);
tlv_status_t config_codec_decode_pwm (const void *   buf,
                                      size_t         len,
                                      pwm_config_t * out);

tlv_status_t config_codec_encode_system (const system_config_t * in,
                                         void *                  buf,
                                         size_t                  cap,
                                         size_t *                out_size);
tlv_status_t config_codec_decode_system (const void *      buf,
                                         size_t            len,
                                         system_config_t * out);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CONFIG_CODEC_H */
