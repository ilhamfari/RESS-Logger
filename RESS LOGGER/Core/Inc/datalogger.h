/**
  * @file    datalogger.h
  * @brief   SD card CSV datalogger. One file per day under 0:/LOGS/ named
  *          YYYYMMDD.CSV (FatFs is configured without long file names, so
  *          the 8.3 form is used instead of YYYY-MM-DD.CSV). A new file gets
  *          a header row; every row is f_sync()'d and the file closed so a
  *          power cut cannot lose the FAT state.
  */

#ifndef DATALOGGER_H
#define DATALOGGER_H

#include "ds3231.h"
#include "power_meter.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DL_DEFAULT_INTERVAL_MIN   1U
#define DL_MAX_INTERVAL_MIN       1440U
#define DL_REMOUNT_RETRY_MS       10000U

void DataLogger_Init(void);

/**
  * @brief Main-loop hook. Logs one row when the interval has elapsed,
  *        the meter is online and the RTC time is valid.
  */
void DataLogger_Process(const DS3231_Time_t *now, bool time_valid,
                        const PowerMeter_Data_t *data, bool meter_online);

/** Force one row immediately (console LOG command). */
bool DataLogger_LogNow(const DS3231_Time_t *now, bool time_valid,
                       const PowerMeter_Data_t *data, bool meter_online);

void     DataLogger_SetIntervalMin(uint32_t minutes);
uint32_t DataLogger_GetIntervalMin(void);

/** FAT timestamp for get_fattime(); 0 until an RTC time has been seen. */
uint32_t DataLogger_GetFatTime(void);
void     DataLogger_UpdateFatTime(const DS3231_Time_t *now);

#ifdef __cplusplus
}
#endif

#endif /* DATALOGGER_H */
