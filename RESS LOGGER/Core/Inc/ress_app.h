/**
  * @file    ress_app.h
  * @brief   Application glue: Modbus polling -> SD datalogger + ESP32 bridge.
  */

#ifndef RESS_APP_H
#define RESS_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Call once after all MX_*_Init() functions. */
void RESS_App_Init(void);

/** Call every main-loop iteration. */
void RESS_App_Process(void);

/* Console helpers (used by at_console.c) */
void RESS_App_PrintMeter(void);
void RESS_App_LogNow(void);
void RESS_App_SetLogInterval(uint32_t minutes);
bool RESS_App_SetTime(uint16_t year, uint8_t month, uint8_t date,
                      uint8_t hour, uint8_t minute, uint8_t second);

#ifdef __cplusplus
}
#endif

#endif /* RESS_APP_H */
