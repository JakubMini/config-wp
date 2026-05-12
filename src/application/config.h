#ifndef APPLICATION_CONFIG_H
#define APPLICATION_CONFIG_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>

#include "application/config_defaults.h"
#include "application/config_types.h"

typedef enum
{
    CONFIG_OK = 0,
    CONFIG_ERR_NOT_FOUND,
    CONFIG_ERR_TOO_LARGE,
    CONFIG_ERR_STORAGE,
    CONFIG_ERR_INVALID,
} config_status_t;

config_status_t config_init (void);

config_status_t config_get (void);

config_status_t config_set (void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CONFIG_H */
