/**
  * @file    ress_app.h
  * @brief   Application glue: Modbus polling (meter + 2 inverters), SD
  *          store-and-forward logging, ESP32 request/response bridge, relays.
  */

#ifndef RESS_APP_H
#define RESS_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "ds3231.h" /* DS3231_Time_t is used as a plain date/time struct */

#ifdef __cplusplus
extern "C" {
#endif

/** Call once after all MX_*_Init() functions. */
void RESS_App_Init(void);

/** Call every main-loop iteration. */
void RESS_App_Process(void);

/* Console helpers (used by at_console.c) */
void RESS_App_PrintMeter(void);
void RESS_App_PrintInverters(void);
void RESS_App_LogNow(void);
bool RESS_App_SetRelay(uint8_t channel, bool on);
bool RESS_App_SetTime(uint16_t year, uint8_t month, uint8_t date,
                      uint8_t hour, uint8_t minute, uint8_t second);

typedef enum
{
    TIME_SRC_NONE = 0,   /* clock never set */
    TIME_SRC_NTP,        /* $CMD,TIME from the ESP32 */
    TIME_SRC_MANUAL      /* console SETTIME */
} RessTimeSource_t;

/* Diagnostics (used by the CHECK command) */
void RESS_App_GetDeviceStatus(bool *meter_on, bool *goodwe_on, bool *sungrow_on);
bool RESS_App_GetTimeInfo(DS3231_Time_t *out_time, bool *is_valid);
RessTimeSource_t RESS_App_GetTimeSource(uint32_t *age_s);

#ifdef __cplusplus
}
#endif

#endif /* RESS_APP_H */
