/*****************************************************************************
 * Module:  config_slot
 * Purpose: Implementation of the A/B-slot persistence protocol. See
 *          config_slot.h for the contract.
 *
 *          On-flash layout:
 *
 *              +-------------------+  offset 0
 *              | Slot A header     |  20 bytes
 *              | Slot A payload    |  up to SLOT_PAYLOAD_MAX bytes
 *              +-------------------+  offset SLOT_TOTAL
 *              | Slot B header     |
 *              | Slot B payload    |
 *              +-------------------+  offset 2 * SLOT_TOTAL
 *
 *          Header (20 bytes, packed):
 *              uint32_t magic;        identifies a slot we wrote
 *              uint16_t format_ver;   layout version of the slot itself
 *              uint16_t flags;        reserved; zero today
 *              uint32_t seq;          monotonic per-write counter
 *              uint32_t length;       payload length in bytes
 *              uint32_t crc32;        CRC over header[..crc32) + payload
 *
 *          The CRC field is placed last so it can cover everything that
 *          precedes it in one streaming pass.
 *
 *          Write order: payload first, then header. The header is the
 *          "commit record" — a torn payload write leaves the slot
 *          rejectable on the next boot, and the other slot remains valid.
 *****************************************************************************/

#include "application/config_slot.h"

#include "application/crc32.h"
#include "drivers/storage.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#define SLOT_MAGIC      ((uint32_t)0xC0FC0FCAu)
#define SLOT_FORMAT_VER ((uint16_t)1u)
#define SLOT_PAYLOAD_MAX \
    ((size_t)2028u) /* tuned so 2*(header+payload) == 4096 */
#define SLOT_HEADER_BYTES ((size_t)20u)
#define SLOT_TOTAL_BYTES  (SLOT_HEADER_BYTES + SLOT_PAYLOAD_MAX)

typedef struct
{
    uint32_t magic;
    uint16_t format_ver;
    uint16_t flags;
    uint32_t seq;
    uint32_t length;
    uint32_t crc32;
} slot_header_t;

static_assert(sizeof(slot_header_t) == SLOT_HEADER_BYTES,
              "slot_header_t must pack to 20 bytes");

static const uint32_t SLOT_OFFSET[2] = { 0u, (uint32_t)SLOT_TOTAL_BYTES };

size_t
slot_max_payload (void)
{
    return SLOT_PAYLOAD_MAX;
}

/* Read a slot's header. Returns true if storage_read succeeded; the caller
 * is still responsible for validating magic / length / CRC of the header. */
static bool
slot_read_header (slot_id_t id, slot_header_t * out)
{
    assert(out != NULL);
    assert(id == SLOT_A || id == SLOT_B);
    if (storage_read(SLOT_OFFSET[id], out, sizeof(*out)) != STORAGE_OK)
    {
        return false;
    }
    return true;
}

/* Validate a header's magic and length only (no payload / CRC read).
 * Used by the write path to learn the current seq cheaply. */
static bool
slot_header_looks_sane (const slot_header_t * hdr)
{
    assert(hdr != NULL);
    if (hdr->magic != SLOT_MAGIC)
    {
        return false;
    }
    if (hdr->length > SLOT_PAYLOAD_MAX)
    {
        return false;
    }
    return true;
}

/* Full validation including CRC over header-prefix + payload. Reads the
 * payload into `buf` (which must hold at least hdr->length bytes). */
static bool
slot_validate_full (slot_id_t             id,
                    const slot_header_t * hdr,
                    void *                buf,
                    size_t                cap)
{
    assert(hdr != NULL);
    assert(buf != NULL);
    if (!slot_header_looks_sane(hdr))
    {
        return false;
    }
    if (hdr->length > cap)
    {
        return false;
    }
    const uint32_t payload_off = SLOT_OFFSET[id] + (uint32_t)SLOT_HEADER_BYTES;
    if (storage_read(payload_off, buf, hdr->length) != STORAGE_OK)
    {
        return false;
    }
    uint32_t state = crc32_start();
    /* CRC covers every byte of the header before the crc32 field, then
     * the payload. offsetof(slot_header_t, crc32) gives the prefix length. */
    state = crc32_step(state, hdr, offsetof(slot_header_t, crc32));
    state = crc32_step(state, buf, hdr->length);
    const uint32_t expected = crc32_end(state);
    return expected == hdr->crc32;
}

slot_status_t
slot_pick_active (slot_id_t * out_id, void * buf, size_t cap, size_t * out_len)
{
    assert(out_id != NULL);
    assert(out_len != NULL);
    if (buf == NULL || cap < SLOT_PAYLOAD_MAX)
    {
        return SLOT_ERR_BUF;
    }

    slot_header_t hdr[2];
    bool          valid[2] = { false, false };

    for (int i = 0; i < 2; ++i)
    {
        if (!slot_read_header((slot_id_t)i, &hdr[i]))
        {
            continue;
        }
        valid[i] = slot_validate_full((slot_id_t)i, &hdr[i], buf, cap);
    }

    int winner = -1;
    if (valid[0] && valid[1])
    {
        winner = (hdr[0].seq >= hdr[1].seq) ? 0 : 1;
    }
    else if (valid[0])
    {
        winner = 0;
    }
    else if (valid[1])
    {
        winner = 1;
    }
    else
    {
        return SLOT_ERR_NO_VALID;
    }

    /* Always re-read the winner's payload. During validation buf is used as
     * scratch for whichever slot was checked last (even if its CRC failed),
     * so it can hold the wrong data here. One extra storage_read on boot
     * is cheap, and the alternative is conditional logic that already
     * tripped us once. */
    const uint32_t payload_off
        = SLOT_OFFSET[winner] + (uint32_t)SLOT_HEADER_BYTES;
    if (storage_read(payload_off, buf, hdr[winner].length) != STORAGE_OK)
    {
        return SLOT_ERR_STORAGE;
    }

    *out_id  = (slot_id_t)winner;
    *out_len = hdr[winner].length;
    return SLOT_OK;
}

slot_status_t
slot_write (const void * payload, size_t len)
{
    assert(payload != NULL || len == 0);
    if (len > SLOT_PAYLOAD_MAX)
    {
        return SLOT_ERR_TOO_LARGE;
    }

    /* Cheap scan: header-only sanity, no payload CRC. We just need to
     * pick the inactive slot and learn a seq to bump past. A torn-write
     * scenario that leaves a slot "sane header, bad payload" still gives
     * us a usable seq — the worst outcome is a slightly-discontiguous
     * seq jump on the next write, which is harmless. */
    slot_header_t hdr[2];
    bool          sane[2] = { false, false };
    for (int i = 0; i < 2; ++i)
    {
        if (slot_read_header((slot_id_t)i, &hdr[i]))
        {
            sane[i] = slot_header_looks_sane(&hdr[i]);
        }
    }

    slot_id_t target  = SLOT_A;
    uint32_t  new_seq = 1u;
    if (sane[0] && sane[1])
    {
        if (hdr[0].seq >= hdr[1].seq)
        {
            target  = SLOT_B;
            new_seq = hdr[0].seq + 1u;
        }
        else
        {
            target  = SLOT_A;
            new_seq = hdr[1].seq + 1u;
        }
    }
    else if (sane[0])
    {
        target  = SLOT_B;
        new_seq = hdr[0].seq + 1u;
    }
    else if (sane[1])
    {
        target  = SLOT_A;
        new_seq = hdr[1].seq + 1u;
    }

    slot_header_t new_hdr;
    new_hdr.magic      = SLOT_MAGIC;
    new_hdr.format_ver = SLOT_FORMAT_VER;
    new_hdr.flags      = 0u;
    new_hdr.seq        = new_seq;
    new_hdr.length     = (uint32_t)len;
    /* crc32 computed below over the header prefix + payload */

    uint32_t state = crc32_start();
    state         = crc32_step(state, &new_hdr, offsetof(slot_header_t, crc32));
    state         = crc32_step(state, payload, len);
    new_hdr.crc32 = crc32_end(state);

    /* Write order: payload first, then header. The header is the commit. */
    const uint32_t payload_off
        = SLOT_OFFSET[target] + (uint32_t)SLOT_HEADER_BYTES;
    if (storage_write(payload_off, payload, len) != STORAGE_OK)
    {
        return SLOT_ERR_STORAGE;
    }
    if (storage_write(SLOT_OFFSET[target], &new_hdr, sizeof(new_hdr))
        != STORAGE_OK)
    {
        return SLOT_ERR_STORAGE;
    }

    return SLOT_OK;
}
