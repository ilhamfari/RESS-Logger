/**
  * @file    ads1115.h
  * @brief   Generic driver for TI ADS1115 16-bit I2C ADC.
  *
  * Single chip on the shared I2C1 bus (SCL=PB6, SDA=PB7), ADDR pin tied to
  * GND -> address 0x48. ALERT/RDY -> PE0 (EXTI0).
  *   - AIN0, AIN1 : 0-10V inputs, through LM324 divider network
  *   - AIN2, AIN3 : 4-20mA inputs, sensed across 150R shunt resistors
  *
  * This file only knows about the chip itself (registers, config bits,
  * raw <-> millivolt conversion). Board-specific channel meaning and
  * engineering-unit scaling live in ress_sensors.c/.h.
  */

#ifndef ADS1115_H
#define ADS1115_H

#include "main.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADS1115_ADDR    (0x48U << 1)  /* ADDR pin tied to GND */

/* Register map */
#define ADS1115_REG_CONVERSION  0x00U
#define ADS1115_REG_CONFIG      0x01U
#define ADS1115_REG_LO_THRESH   0x02U
#define ADS1115_REG_HI_THRESH   0x03U

/* CONFIG register field values (see ADS1115 datasheet Table 8) */
#define ADS1115_OS_SINGLE_START     (1U << 15)

#define ADS1115_MUX_AIN0_GND        (4U << 12)
#define ADS1115_MUX_AIN1_GND        (5U << 12)
#define ADS1115_MUX_AIN2_GND        (6U << 12)
#define ADS1115_MUX_AIN3_GND        (7U << 12)

/* PGA: full-scale range. 0-10V input must already be attenuated by the
 * on-board precision resistor divider to fit within +-4.096V. */
#define ADS1115_PGA_6144MV          (0U << 9)
#define ADS1115_PGA_4096MV          (1U << 9)
#define ADS1115_PGA_2048MV          (2U << 9)
#define ADS1115_PGA_1024MV          (3U << 9)
#define ADS1115_PGA_0512MV          (4U << 9)
#define ADS1115_PGA_0256MV          (5U << 9)

#define ADS1115_MODE_SINGLE_SHOT    (1U << 8)

#define ADS1115_DR_128SPS           (4U << 5)

#define ADS1115_COMP_MODE_TRAD      (0U << 4)
#define ADS1115_COMP_POL_ACTIVE_LOW (0U << 3)
#define ADS1115_COMP_LAT_DISABLE    (0U << 2)
#define ADS1115_COMP_QUE_ONE        (0U << 0)  /* assert ALERT/RDY after 1 conversion */
#define ADS1115_COMP_QUE_DISABLE    (3U << 0)  /* disable comparator / ALERT-RDY function */

typedef enum
{
    ADS1115_CH0 = 0,
    ADS1115_CH1 = 1,
    ADS1115_CH2 = 2,
    ADS1115_CH3 = 3,
} ADS1115_Channel_t;

typedef struct
{
    I2C_HandleTypeDef *hi2c;
    uint16_t           i2c_addr;   /* already left-shifted 8-bit HAL address */
    uint16_t           pga_config; /* one of ADS1115_PGA_* */
} ADS1115_Handle_t;

/**
  * @brief Bind a driver instance to an I2C bus/address and PGA range.
  */
void ADS1115_Init(ADS1115_Handle_t *dev, I2C_HandleTypeDef *hi2c, uint16_t i2c_addr, uint16_t pga_config);

/**
  * @brief Configure ALERT/RDY pin to pulse once per completed single-shot
  *        conversion (conversion-ready mode), per datasheet section on
  *        using ALERT/RDY as a conversion-ready pin: Hi_thresh MSB=1,
  *        Lo_thresh MSB=0, COMP_QUE != 11.
  */
HAL_StatusTypeDef ADS1115_ConfigConversionReadyPin(ADS1115_Handle_t *dev);

/**
  * @brief Start a single-shot conversion on the given channel (single-ended,
  *        vs GND). Non-blocking: ALERT/RDY (EXTI) or polling tells you when done.
  */
HAL_StatusTypeDef ADS1115_StartSingleConversion(ADS1115_Handle_t *dev, ADS1115_Channel_t channel);

/**
  * @brief Read the last conversion result (raw signed 16-bit code).
  */
HAL_StatusTypeDef ADS1115_ReadRaw(ADS1115_Handle_t *dev, int16_t *out_raw);

/**
  * @brief Convert a raw code to millivolts at the ADC pin, based on dev->pga_config.
  *        NOTE: this is the voltage AT THE ADS1115 PIN, after the board's
  *        precision resistor divider (0-10V) or shunt (4-20mA) -- it is NOT
  *        yet scaled back to the original 0-10V / 4-20mA field signal.
  *        Apply your divider ratio / shunt resistance on top of this.
  */
int32_t ADS1115_RawToMillivolts(const ADS1115_Handle_t *dev, int16_t raw);

#ifdef __cplusplus
}
#endif

#endif /* ADS1115_H */
