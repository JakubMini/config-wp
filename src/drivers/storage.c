#include "drivers/storage.h"

#include <string.h>

/* Skeleton in-memory backing for the POSIX dev environment. On real
 * hardware this would talk to flash / EEPROM / FRAM. Replace this with
 * whatever fits the configuration management design. */

#define STORAGE_BYTES (4 * 1024)

static uint8_t s_backing[STORAGE_BYTES];
static int     s_initialised;

storage_status_t
storage_init (void)
{
    memset(s_backing, 0xFF, sizeof(s_backing));
    s_initialised = 1;
    return STORAGE_OK;
}

storage_status_t
storage_read (uint32_t offset, void * buf, size_t len)
{
    if (!s_initialised || buf == NULL)
    {
        return STORAGE_ERR_IO;
    }
    if (offset + len > sizeof(s_backing))
    {
        return STORAGE_ERR_RANGE;
    }
    memcpy(buf, &s_backing[offset], len);
    return STORAGE_OK;
}

storage_status_t
storage_write (uint32_t offset, const void * buf, size_t len)
{
    if (!s_initialised || buf == NULL)
    {
        return STORAGE_ERR_IO;
    }
    if (offset + len > sizeof(s_backing))
    {
        return STORAGE_ERR_RANGE;
    }
    memcpy(&s_backing[offset], buf, len);
    return STORAGE_OK;
}

size_t
storage_size (void)
{
    return sizeof(s_backing);
}
