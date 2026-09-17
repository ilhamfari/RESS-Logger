/**
  * @file    ds3231.h
  * @brief   Driver for Maxim DS3231M RTC (common breakout module),
  *          on shared I2C1 bus. Fixed I2C address 0x68.
  */

#ifndef DS3231_H
#define DS3231_H

#include "main.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS3231_ADDR   (0x68U << 1)

typedef struct
{
    uint16_t year;   /* full year, e.g. 2026 */
    uint8_t  month;  /* 1-12 */
    uint8_t  date;   /* 1-31 */
    uint8_t  hour;   /* 0-23 (always 24h mode) */
    uint8_t  minute; /* 0-59 */
    uint8_t  second; /* 0-59 */
} DS3231_Time_t;

typedef struct
{
    I2C_HandleTypeDef *hi2c;
} DS3231_Handle_t;

void DS3231_Init(DS3231_Handle_t *dev, I2C_HandleTypeDef *hi2c);

/**
  * @brief Read the current time/date. Returns HAL_ERROR if the oscillator
  *        stop flag (OSF) is set, meaning the RTC lost power/backup battery
  *        and the time is not trustworthy -- caller should treat this as
  *        "no valid time" (e.g. flag logged records, or block logging
  *        until the time is reset).
  */
HAL_StatusTypeDef DS3231_GetTime(DS3231_Handle_t *dev, DS3231_Time_t *out);

/**
  * @brief Set the current time/date (e.g. from a provisioning tool at
  *        deployment, or NTP-derived time if a network link is up).
  */
HAL_StatusTypeDef DS3231_SetTime(DS3231_Handle_t *dev, const DS3231_Time_t *time);

/**
  * @brief Read chip temperature (integrated sensor, 0.25 degC resolution).
  */
HAL_StatusTypeDef DS3231_GetTemperature(DS3231_Handle_t *dev, float *out_degC);

#ifdef __cplusplus
}
#endif

#endif /* DS3231_H */
