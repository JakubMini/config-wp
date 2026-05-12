/*****************************************************************************
 * Module:  tlv
 * Purpose: Implementation of the generic TLV iterator and writer. See
 *          tlv.h for the contract.
 *
 *          Wire integers are written and read in little-endian byte order
 *          explicitly, so the on-flash format is independent of host /
 *          target byte order. Cortex-M and the host clang build are both
 *          little-endian today, but the explicit shifts mean a big-endian
 *          host (e.g. a future cross-compile to MIPS) would still produce
 *          identical bytes on the wire.
 *****************************************************************************/

#include "application/tlv.h"

#include <assert.h>
#include <string.h>

static uint16_t
read_u16_le (const uint8_t * p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void
write_u16_le (uint8_t * p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* --------------------------------------------------------------------- */
/* Iterator                                                              */
/* --------------------------------------------------------------------- */

void
tlv_iter_init (tlv_iter_t * it, const void * buf, size_t len)
{
    assert(it != NULL);
    assert(buf != NULL || len == 0);
    it->buf     = (const uint8_t *)buf;
    it->buf_len = len;
    it->cursor  = 0;
}

bool
tlv_iter_done (const tlv_iter_t * it)
{
    assert(it != NULL);
    return it->cursor >= it->buf_len;
}

tlv_status_t
tlv_iter_next (tlv_iter_t *  it,
               uint16_t *    out_tag,
               const void ** out_value,
               size_t *      out_value_len)
{
    assert(it != NULL);
    assert(out_tag != NULL);
    assert(out_value != NULL);
    assert(out_value_len != NULL);

    if (it->cursor == it->buf_len)
    {
        return TLV_END;
    }
    /* Need at least a full header before the next record. A partial
     * header at the end of the buffer is malformed, not "end of stream". */
    if (it->cursor + TLV_HEADER_BYTES > it->buf_len)
    {
        return TLV_ERR_TRUNCATED;
    }

    const uint16_t tag    = read_u16_le(&it->buf[it->cursor]);
    const uint16_t length = read_u16_le(&it->buf[it->cursor + 2u]);

    const size_t value_off = it->cursor + TLV_HEADER_BYTES;
    if (value_off + length > it->buf_len)
    {
        return TLV_ERR_OVERRUN;
    }

    *out_tag       = tag;
    *out_value     = &it->buf[value_off];
    *out_value_len = (size_t)length;
    it->cursor     = value_off + length;
    return TLV_OK;
}

/* --------------------------------------------------------------------- */
/* Writer                                                                */
/* --------------------------------------------------------------------- */

void
tlv_writer_init (tlv_writer_t * w, void * buf, size_t cap)
{
    assert(w != NULL);
    assert(buf != NULL || cap == 0);
    w->buf  = (uint8_t *)buf;
    w->cap  = cap;
    w->used = 0;
}

size_t
tlv_writer_size (const tlv_writer_t * w)
{
    assert(w != NULL);
    return w->used;
}

tlv_status_t
tlv_writer_emit (tlv_writer_t * w,
                 uint16_t       tag,
                 const void *   value,
                 size_t         value_len)
{
    assert(w != NULL);
    if (value == NULL && value_len != 0u)
    {
        return TLV_ERR_BUF;
    }
    if (value_len > (size_t)UINT16_MAX)
    {
        return TLV_ERR_TOO_BIG;
    }
    if (w->used + TLV_HEADER_BYTES + value_len > w->cap)
    {
        return TLV_ERR_NO_SPACE;
    }

    write_u16_le(&w->buf[w->used], tag);
    write_u16_le(&w->buf[w->used + 2u], (uint16_t)value_len);
    if (value_len != 0u)
    {
        memcpy(&w->buf[w->used + TLV_HEADER_BYTES], value, value_len);
    }
    w->used += TLV_HEADER_BYTES + value_len;
    return TLV_OK;
}

tlv_status_t
tlv_writer_emit_raw (tlv_writer_t * w, const void * record, size_t total_len)
{
    assert(w != NULL);
    if (record == NULL || total_len < TLV_HEADER_BYTES)
    {
        return TLV_ERR_BUF;
    }
    /* Sanity-check the embedded length so we don't blindly accept a
     * malformed raw record. */
    const uint8_t * src    = (const uint8_t *)record;
    const uint16_t  length = read_u16_le(&src[2]);
    if ((size_t)TLV_HEADER_BYTES + (size_t)length != total_len)
    {
        return TLV_ERR_OVERRUN;
    }
    if (w->used + total_len > w->cap)
    {
        return TLV_ERR_NO_SPACE;
    }
    memcpy(&w->buf[w->used], record, total_len);
    w->used += total_len;
    return TLV_OK;
}
