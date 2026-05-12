/*****************************************************************************
 * Module:  config_print
 * Purpose: stdio pretty-printers for the configuration manager. Used by the
 *          demo flow in main.c to show the manager doing its thing on stdout.
 *
 *          Lives outside the `application` library on purpose — only the
 *          executable links it, so unit tests don't pull in stdio noise.
 *
 *          No state. No allocation. Safe to call from any task.
 *****************************************************************************/

#ifndef CONFIG_PRINT_H
#define CONFIG_PRINT_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>

#include "application/config.h"
#include "application/config_types.h"

/* Status -> short string ("OK", "ERR_INDEX", ...). Never NULL. */
const char * config_print_status (config_status_t s);

/* Print the current system block under a labelled banner. Reads via
 * config_get_system(); on failure logs the status and returns. */
void config_print_system (const char * label);

/* Print one DI record under a labelled banner. Reads via config_get_di();
 * on failure logs the status and returns. */
void config_print_di (uint8_t idx);

/* Print a delimited stage banner used to chunk the demo flow. */
void config_print_stage (const char * title);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_PRINT_H */
