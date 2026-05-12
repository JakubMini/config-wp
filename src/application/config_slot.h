/*****************************************************************************
 * Module:  config_slot
 * Purpose: A/B-slot persistence protocol sitting on top of drivers/storage.
 *
 *          The medium below is treated as an opaque byte buffer (read/write
 *          at arbitrary offsets). The slot layer carves out two equal-sized
 *          slots from the front of that buffer; each slot stores one
 *          revision of the configuration payload together with a header
 *          that carries a sequence number and a CRC-32 covering the header
 *          prefix and the payload.
 *
 *          Properties:
 *            - A power-loss mid-write corrupts at most one slot; the other
 *              survives and remains the valid configuration.
 *            - A bit-flip or torn write fails the CRC; that slot is
 *              ignored on the next boot.
 *            - When both slots are valid the newer one (higher seq) wins.
 *            - A brand-new device (storage all 0xFF) reads as "no valid
 *              slot"; the caller falls back to factory defaults.
 *
 *          The slot layer is intentionally type-agnostic — it shuttles
 *          opaque byte payloads. No dependency on config_types.h.
 *****************************************************************************/

#ifndef APPLICATION_CONFIG_SLOT_H
#define APPLICATION_CONFIG_SLOT_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    SLOT_OK = 0,
    SLOT_ERR_NO_VALID,  /* both slots blank / corrupt; storage I/O was OK */
    SLOT_ERR_TOO_LARGE, /* payload exceeds slot_max_payload() */
    SLOT_ERR_STORAGE,   /* underlying storage_read / storage_write failed */
    SLOT_ERR_BUF,       /* caller's buffer is NULL, too small, or len == 0 */
} slot_status_t;

typedef enum
{
    SLOT_A    = 0,
    SLOT_B    = 1,
    SLOT_NONE = 2,
} slot_id_t;

/* Maximum payload bytes the slot layer accepts per write / returns per
 * read. Compile-time constant so callers can statically size buffers. */
size_t slot_max_payload (void);

/* Locate the valid slot with the highest sequence number, copy its
 * payload into the caller-provided buffer, and report which slot it
 * came from.
 *
 *   - On success: *out_id is SLOT_A or SLOT_B, payload is in buf,
 *                 *out_len is the payload length in bytes.
 *   - If both slots are blank / corrupt: returns SLOT_ERR_NO_VALID and
 *     leaves the caller to fall back to factory defaults.
 *
 * buf is used as scratch during the scan; cap must be >= slot_max_payload(). */
slot_status_t slot_pick_active (slot_id_t * out_id,
                                void *      buf,
                                size_t      cap,
                                size_t *    out_len);

/* Write a payload to the inactive slot, bumping the sequence number past
 * the current active. After this call the just-written slot becomes the
 * new active. If no slot is currently active (blank EEPROM, both
 * corrupt) the write lands in slot A with seq = 1. */
slot_status_t slot_write (const void * payload, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CONFIG_SLOT_H */
