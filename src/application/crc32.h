/*****************************************************************************
 * Module:  crc32
 * Purpose: CRC-32/ISO-HDLC ("zlib's crc32") — polynomial 0xEDB88320 reflected,
 *          init 0xFFFFFFFF, final XOR 0xFFFFFFFF. Same algorithm as zlib,
 *          gzip, PNG, and Ethernet, and matches the STM32 hardware CRC
 *          peripheral when configured for the standard polynomial.
 *
 *          Two APIs:
 *            - One-shot: crc32_compute(buf, len)
 *            - Streaming: crc32_start() -> crc32_step()* -> crc32_end()
 *
 *          Streaming is used by the slot layer so the CRC can span the
 *          header-prefix and the payload in one pass without concatenating
 *          them in memory.
 *
 *          The 256-entry lookup table is built lazily on first use. Callers
 *          may force the build with crc32_init() at startup if they prefer
 *          deterministic init-time cost.
 *****************************************************************************/

#ifndef APPLICATION_CRC32_H
#define APPLICATION_CRC32_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>

/* Build the 256-entry lookup table. Safe to call more than once. Optional
 * — the streaming and one-shot APIs build the table lazily if needed. */
void crc32_init (void);

/* One-shot CRC over `len` bytes at `data`. Returns the finalised CRC. */
uint32_t crc32_compute (const void * data, size_t len);

/* Streaming API. Use when the data is not contiguous in memory. */
uint32_t crc32_start (void);
uint32_t crc32_step (uint32_t state, const void * data, size_t len);
uint32_t crc32_end (uint32_t state);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CRC32_H */
