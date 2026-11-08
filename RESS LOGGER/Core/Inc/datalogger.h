/**
  * @file    datalogger.h
  * @brief   SD card CSV datalogger with store-and-forward tracking.
  *          One file per day under 0:/LOGS/ named YYYYMMDD.CSV (FatFs has
  *          no long file names, so the 8.3 form is used). One row per
  *          snapshot, last column SENT = 0 until the ESP32 confirms MQTT
  *          delivery, then DataLogger_MarkSent() rewrites it to 1 in place.
  */

#ifndef DATALOGGER_H
#define DATALOGGER_H

#include "ress_snapshot.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DL_REMOUNT_RETRY_MS       10000U
#define DL_PENDING_MAX            8U     /* rows whose SENT flag can still be updated */

void DataLogger_Init(void);

/** Append one row (SENT=0). Mounts the card if needed. */
bool DataLogger_WriteSnapshot(const RessSnapshot_t *snap);

/** Rewrite SENT to 1 for a previously written row. */
bool DataLogger_MarkSent(uint32_t seq);

uint32_t DataLogger_GetRowsWritten(void);
uint32_t DataLogger_GetRowsSent(void);
uint8_t  DataLogger_GetPendingCount(void);
bool     DataLogger_IsMounted(void);

/** FAT timestamp for get_fattime(); 0 until an RTC time has been seen. */
uint32_t DataLogger_GetFatTime(void);
void     DataLogger_UpdateFatTime(const DS3231_Time_t *now);

#ifdef __cplusplus
}
#endif

#endif /* DATALOGGER_H */
