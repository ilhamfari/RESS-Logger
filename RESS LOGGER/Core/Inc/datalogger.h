/**
  * @file    datalogger.h
  * @brief   SD card CSV datalogger with persistent store-and-forward.
  *          Daily CSV files live under 0:/GWLOG2/. Each unsent row also has
  *          a durable queue record under 0:/GWPEND/ so replay survives reset.
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
void DataLogger_Init(void);

/** Append one row (SENT=0). Mounts the card if needed. */
bool DataLogger_WriteSnapshot(const RessSnapshot_t *snap);

/** Rewrite SENT to 1 for a previously written row. */
bool DataLogger_MarkSent(uint32_t seq);

/** Load the oldest durable unsent snapshot. Returns false when the queue is empty. */
bool DataLogger_LoadOldestPending(RessSnapshot_t *out);

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
