/*****************************************************************************
 * Module:  config_limits
 * Purpose: Compile-time configuration limits — channel counts per IO type
 *          and the name string length. Single source of truth so anything
 *          that needs to size an array or check a bound includes this file.
 *
 *          To grow channel counts, bump the number here and recompile.
 *          Adding a whole new IO type means editing this file plus the
 *          associated struct, defaults table, and codec entries.
 *
 *          Sources (from the hardware diagram):
 *              16 DI / 16 DO       2 x PCA9555 each
 *              4  TC               MAX31856 array
 *              8  AI               ADC1.IN1..IN8 on the STM32G
 *              4  AO               DAC channels (conservative placeholder)
 *              4  PCNT             TIM2.CH1..CH4
 *              4  PWM              TIMn.CH1..CH4
 *****************************************************************************/

#ifndef APPLICATION_CONFIG_LIMITS_H
#define APPLICATION_CONFIG_LIMITS_H

#define CONFIG_NAME_LEN 16

#define CONFIG_NUM_DI   16
#define CONFIG_NUM_DO   16
#define CONFIG_NUM_TC   4
#define CONFIG_NUM_AI   8
#define CONFIG_NUM_AO   4
#define CONFIG_NUM_PCNT 4
#define CONFIG_NUM_PWM  4

#endif /* APPLICATION_CONFIG_LIMITS_H */
