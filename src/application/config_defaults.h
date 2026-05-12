/*****************************************************************************
 * Module:  config_defaults
 * Purpose: Factory-default tables for every IO type and the system config.
 *
 *          The tables here are the ground truth used when:
 *            1. EEPROM is fresh (magic mismatch on both slots)
 *            2. config_reset_defaults() is invoked
 *            3. A new field appears in a struct after a firmware upgrade
 *               and the loaded record had no value for it
 *
 *          Tables are static const so they live in flash on the target, not
 *          RAM. Callers must treat them as read-only.
 *****************************************************************************/

#ifndef APPLICATION_CONFIG_DEFAULTS_H
#define APPLICATION_CONFIG_DEFAULTS_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "application/config_limits.h"
#include "application/config_types.h"

extern const di_config_t   g_di_defaults[CONFIG_NUM_DI];
extern const do_config_t   g_do_defaults[CONFIG_NUM_DO];
extern const tc_config_t   g_tc_defaults[CONFIG_NUM_TC];
extern const ai_config_t   g_ai_defaults[CONFIG_NUM_AI];
extern const ao_config_t   g_ao_defaults[CONFIG_NUM_AO];
extern const pcnt_config_t g_pcnt_defaults[CONFIG_NUM_PCNT];
extern const pwm_config_t  g_pwm_defaults[CONFIG_NUM_PWM];

extern const system_config_t g_system_defaults;

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_CONFIG_DEFAULTS_H */
