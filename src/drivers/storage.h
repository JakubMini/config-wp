#ifndef DRIVERS_STORAGE_H
#define DRIVERS_STORAGE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    STORAGE_OK = 0,
    STORAGE_ERR_IO,
    STORAGE_ERR_RANGE,
} storage_status_t;

/* Initialise the underlying non-volatile storage. */
storage_status_t storage_init (void);

/* Read `len` bytes from `offset` into `buf`. */
storage_status_t storage_read (uint32_t offset, void * buf, size_t len);

/* Write `len` bytes from `buf` at `offset`. */
storage_status_t storage_write (uint32_t offset, const void * buf, size_t len);

/* Total addressable size in bytes. */
size_t storage_size (void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_STORAGE_H */
