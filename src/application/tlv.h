/*****************************************************************************
 * Module:  tlv
 * Purpose: Generic Tag-Length-Value record codec. Knows nothing about
 *          configuration types — operates on opaque byte payloads.
 *
 *          Record format (all integers little-endian on the wire):
 *
 *              +-----------+-----------+----------------------+
 *              |   tag     |  length   | value (length bytes) |
 *              +-----------+-----------+----------------------+
 *               2 bytes      2 bytes       up to 65535 bytes
 *
 *          A TLV stream is a concatenation of records with no padding.
 *          Iterating: read tag, read length, hand the caller a pointer to
 *          the value, advance by `length` whether or not the caller
 *          recognised the tag. That's the property that gives us
 *          forward-compatibility — unknown records cost a `length` field
 *          to skip and zero parsing work.
 *
 *          Writing: pre-flight space, emit the 4-byte header, copy the
 *          value bytes. Records appended in the order the writer was
 *          called.
 *
 *          No allocation. No interpretation. Tag-name and value-shape
 *          conventions are in config_codec.{h,c}.
 *****************************************************************************/

#ifndef APPLICATION_TLV_H
#define APPLICATION_TLV_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    TLV_OK = 0,
    TLV_END,           /* iter: no more records (end of stream) */
    TLV_ERR_TRUNCATED, /* iter: partial header at end of stream */
    TLV_ERR_OVERRUN,   /* iter: record length walks off the buffer */
    TLV_ERR_TOO_BIG,   /* writer: value exceeds UINT16_MAX bytes */
    TLV_ERR_NO_SPACE,  /* writer: not enough room for the record */
    TLV_ERR_BUF,       /* either: NULL pointer / zero capacity */
} tlv_status_t;

/* TLV header size on the wire: u16 tag + u16 length. */
#define TLV_HEADER_BYTES 4u

/* --------------------------------------------------------------------- */
/* Iterator (decoder)                                                    */
/* --------------------------------------------------------------------- */

typedef struct
{
    const uint8_t * buf;
    size_t          buf_len;
    size_t          cursor;
} tlv_iter_t;

void tlv_iter_init (tlv_iter_t * it, const void * buf, size_t len);

/* Read the next record. On TLV_OK, *out_tag and *out_value_len hold the
 * record header, and *out_value points into the underlying buffer (no
 * copy). On TLV_END the stream is consumed cleanly; on any other status
 * the buffer is malformed and iteration must stop. */
tlv_status_t tlv_iter_next (tlv_iter_t *  it,
                            uint16_t *    out_tag,
                            const void ** out_value,
                            size_t *      out_value_len);

bool tlv_iter_done (const tlv_iter_t * it);

/* --------------------------------------------------------------------- */
/* Writer (encoder)                                                      */
/* --------------------------------------------------------------------- */

typedef struct
{
    uint8_t * buf;
    size_t    cap;
    size_t    used;
} tlv_writer_t;

void tlv_writer_init (tlv_writer_t * w, void * buf, size_t cap);

/* Append a single record. Records land in the buffer in the order
 * they're emitted. */
tlv_status_t tlv_writer_emit (tlv_writer_t * w,
                              uint16_t       tag,
                              const void *   value,
                              size_t         value_len);

/* Append a raw, pre-serialised TLV record (header + value, e.g. one
 * preserved from a prior decode). The first 4 bytes must be a valid
 * little-endian (tag, length) pair and total_len must equal
 * TLV_HEADER_BYTES + the declared length. Used to round-trip unknown
 * tags without interpreting them. */
tlv_status_t tlv_writer_emit_raw (tlv_writer_t * w,
                                  const void *   record,
                                  size_t         total_len);

size_t tlv_writer_size (const tlv_writer_t * w);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_TLV_H */
