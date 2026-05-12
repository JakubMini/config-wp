/*****************************************************************************
 * Module:  config
 * Purpose: Public API for the configuration manager. Holds the in-RAM
 *          cache, serialises access via a mutex, and ties together the
 *          codec + slot layers so a single config_init() loads the
 *          latest slot (or factory defaults) and config_save() persists
 *          the current cache.
 *
 *          API shape:
 *            - lifecycle: config_init / config_deinit /
 *              config_reset_defaults / config_save
 *            - per-IO-type: config_get_<type> / config_set_<type>
 *            - system-wide: config_get_system / config_set_system
 *
 *          Threading:
 *            - All getters and setters are safe for concurrent use from
 *              multiple FreeRTOS tasks. The manager takes a single
 *              non-recursive mutex on entry and releases it before
 *              returning.
 *            - config_save() releases the mutex before calling slot_write
 *              so the SPI/EEPROM transaction does not block readers.
 *              Caller contract: only one thread calls config_save() at
 *              a time (typically the EEPROM Manager in the firmware
 *              architecture).
 *****************************************************************************/

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
    CONFIG_ERR_NOT_INITIALISED, /* api called before config_init */
    CONFIG_ERR_INDEX,           /* idx out of range for the type */
    CONFIG_ERR_INVALID,         /* value out of range or NULL pointer */
    CONFIG_ERR_STORAGE,         /* slot_write / slot_pick_active failed */
    CONFIG_ERR_CODEC,           /* tlv encode/decode error */
    CONFIG_ERR_TOO_LARGE,       /* encoded cache exceeds slot_max_payload */
    CONFIG_ERR_INTERNAL, /* manager internal failure (mutex create, etc.) */
} config_status_t;

/* ---- Lifecycle ---------------------------------------------------- */

/* Initialise the manager. Sets up the mutex, the CRC32 lookup, and
 * populates the cache from factory defaults. Then reads the latest
 * valid slot (if any) and overlays the loaded values on top of the
 * defaults. Unknown TLV records in the loaded blob are stashed for
 * verbatim re-emission on the next save.
 *
 * Public API stays gated until decode completes — until then every
 * getter/setter returns CONFIG_ERR_NOT_INITIALISED, so no other task
 * can observe a half-loaded cache.
 *
 * Returns:
 *   CONFIG_OK                  success or blank EEPROM (defaults loaded)
 *   CONFIG_ERR_INTERNAL        mutex create failed (lock primitive)
 *   CONFIG_ERR_STORAGE         storage I/O failed; defaults loaded so
 *                              the device can still run in a degraded mode
 *   CONFIG_ERR_CODEC           slot bytes failed to decode; defaults loaded
 *
 * Idempotent: subsequent calls return CONFIG_OK without reloading.
 * Use config_deinit() to force a re-load. Caller contract: config_init
 * is called from one thread at startup. */
config_status_t config_init (void);

/* Tear down manager state. Primarily for tests; production code rarely
 * calls this. Safe to call when not initialised. */
void config_deinit (void);

/* Reset the entire cache to factory defaults. Does NOT persist — call
 * config_save() to commit. Discards any preserved unknown TLV records. */
config_status_t config_reset_defaults (void);

/* Encode the cache (and any preserved unknown TLV records) into a slot
 * payload and call slot_write(). The mutex is released before slot_write
 * so the storage I/O does not block other readers. */
config_status_t config_save (void);

/* ---- Per-type getters and setters ---------------------------------
 *
 * Every setter validates the input before taking the mutex:
 *   - NULL pointer  -> CONFIG_ERR_INVALID
 *   - idx >= count  -> CONFIG_ERR_INDEX
 *   - field out of range / bad enum / bad invariant -> CONFIG_ERR_INVALID
 *
 * The struct in the cache is replaced atomically (under the mutex).
 * Setters do NOT persist — call config_save() to write to storage. */

config_status_t config_get_di (uint8_t idx, di_config_t * out);
config_status_t config_set_di (uint8_t idx, const di_config_t * in);

config_status_t config_get_do (uint8_t idx, do_config_t * out);
config_status_t config_set_do (uint8_t idx, const do_config_t * in);

config_status_t config_get_tc (uint8_t idx, tc_config_t * out);
config_status_t config_set_tc (uint8_t idx, const tc_config_t * in);

config_status_t config_get_ai (uint8_t idx, ai_config_t * out);
config_status_t config_set_ai (uint8_t idx, const ai_config_t * in);

config_status_t config_get_ao (uint8_t idx, ao_config_t * out);
config_status_t config_set_ao (uint8_t idx, const ao_config_t * in);

config_status_t config_get_pcnt (uint8_t idx, pcnt_config_t * out);
config_status_t config_set_pcnt (uint8_t idx, const pcnt_config_t * in);

config_status_t config_get_pwm (uint8_t idx, pwm_config_t * out);
config_status_t config_set_pwm (uint8_t idx, const pwm_config_t * in);

config_status_t config_get_system (system_config_t * out);
config_status_t config_set_system (const system_config_t * in);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CONFIG_H */
