/**
  * @file    ress_sensors.h
  * @brief   Board-specific mapping of the single ADS1115's 4 channels to
  *          the logger's 0-10V and 4-20mA field inputs, with engineering
  *          unit conversion.
  *
  * Channel map (per schematic):
  *   AIN0 -> 0-10V input #1  (LM324 divider: R2=175k, R5=75k to GND)
  *   AIN1 -> 0-10V input #2  (same divider network)
  *   AIN2 -> 4-20mA input #1 (150R shunt, R14)
  *   AIN3 -> 4-20mA input #2 (150R shunt, R18)
  *
  * Divider ratio for the 0-10V channels is derived from R2/R5 assuming
  * the LM324 stage is a unity-gain buffer following a resistive divider
  * (Vout = Vin * R5/(R2+R5)). VERIFY against the actual schematic net
  * list / measure it on hardware before trusting logged values -- the
  * op-amp feedback routing was inferred from the pinout doc, not traced.
  */

#ifndef RESS_SENSORS_H
#define RESS_SENSORS_H

#include "ads1115.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    RESS_CH_VOLTAGE_0 = ADS1115_CH0, /* 0-10V input #1 */
    RESS_CH_VOLTAGE_1 = ADS1115_CH1, /* 0-10V input #2 */
    RESS_CH_CURRENT_0 = ADS1115_CH2, /* 4-20mA input #1 */
    RESS_CH_CURRENT_1 = ADS1115_CH3, /* 4-20mA input #2 */
    RESS_CH_COUNT     = 4,
} RESS_Channel_t;

/* Latest scaled reading per channel, updated by RESS_Sensors_OnConversionReady() */
typedef struct
{
    float voltage_0_V;   /* field voltage, channel AIN0, volts */
    float voltage_1_V;   /* field voltage, channel AIN1, volts */
    float current_0_mA;  /* field current, channel AIN2, milliamps */
    float current_1_mA;  /* field current, channel AIN3, milliamps */
    bool  valid;          /* true once every channel has been read at least once */
} RESS_SensorData_t;

/**
  * @brief Init the ADS1115 handle and start the round-robin conversion
  *        cycle on AIN0. Call once after MX_I2C1_Init().
  */
void RESS_Sensors_Init(I2C_HandleTypeDef *hi2c);

/**
  * @brief Call from HAL_GPIO_EXTI_Callback() when the ADS1115 ALERT/RDY
  *        pin (PE0 / EXTI0) fires. Reads the just-completed conversion,
  *        stores the scaled result, and starts the next channel's
  *        conversion (round-robin AIN0 -> AIN1 -> AIN2 -> AIN3 -> AIN0...).
  */
void RESS_Sensors_OnConversionReady(void);

/**
  * @brief Get a snapshot of the latest readings. Safe to call from the
  *        main loop; does not block on I2C.
  */
void RESS_Sensors_GetLatest(RESS_SensorData_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RESS_SENSORS_H */
