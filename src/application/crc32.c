/*****************************************************************************
 * Module:  crc32
 * Purpose: Implementation of CRC-32/ISO-HDLC. See crc32.h for contract.
 *
 *          The lookup table is built once on first use via a single-byte
 *          flag. No threading concerns for the build step itself — the
 *          worst case is two threads racing to fill the same deterministic
 *          values, which is benign.
 *****************************************************************************/

#include "application/crc32.h"

#include <assert.h>
#include <stdbool.h>

#define CRC32_POLY    ((uint32_t)0xEDB88320u)
#define CRC32_INITIAL ((uint32_t)0xFFFFFFFFu)
#define CRC32_FINAL   ((uint32_t)0xFFFFFFFFu)

static uint32_t s_table[256];
static bool     s_table_ready = false;

void
crc32_init (void)
{
    for (uint32_t i = 0; i < 256u; ++i)
    {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
        {
            c = (c >> 1) ^ ((c & 1u) ? CRC32_POLY : 0u);
        }
        s_table[i] = c;
    }
    s_table_ready = true;
    assert(s_table[0] == 0u);
    assert(s_table[1] == 0x77073096u);
}

uint32_t
crc32_start (void)
{
    return CRC32_INITIAL;
}

uint32_t
crc32_step (uint32_t state, const void * data, size_t len)
{
    assert(data != NULL || len == 0);
    if (!s_table_ready)
    {
        crc32_init();
    }
    const uint8_t * p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i)
    {
        state = s_table[(state ^ p[i]) & 0xFFu] ^ (state >> 8);
    }
    return state;
}

uint32_t
crc32_end (uint32_t state)
{
    return state ^ CRC32_FINAL;
}

uint32_t
crc32_compute (const void * data, size_t len)
{
    assert(data != NULL || len == 0);
    return crc32_end(crc32_step(crc32_start(), data, len));
}
