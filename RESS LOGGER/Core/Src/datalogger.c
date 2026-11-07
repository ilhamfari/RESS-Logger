#include "datalogger.h"
#include "fatfs.h"
#include "iwdg.h"
#include "sd_spi.h"
#include "ress_util.h"
#include "ress_config.h"
#include <string.h>

#define CSV_HEADER \
    "timestamp,device_id,ua,ub,uc,uab,ubc,uca,ia,ib,ic,pa,pb,pc,p_total," \
    "qa,qb,qc,q_total,sa,sb,sc,s_total,pf1,pf2,pf3,pf_avg,freq\r\n"

static uint32_t s_interval_min = DL_DEFAULT_INTERVAL_MIN;
static uint32_t s_last_log_tick;
static bool     s_logged_once;
static bool     s_mounted;
static uint32_t s_last_mount_try;
static bool     s_mount_tried;
static uint32_t s_fattime;
static uint32_t s_rows_written;
static uint32_t s_last_skip_print;

void DataLogger_Init(void)
{
    s_interval_min = DL_DEFAULT_INTERVAL_MIN;
    s_logged_once = false;
    s_mounted = false;
    s_mount_tried = false;
    s_rows_written = 0;
    RESS_LOG("LOGGER", "init: interval=%lu min, dir=%sLOGS, file=YYYYMMDD.CSV",
             (unsigned long)s_interval_min, USERPath);
}

void DataLogger_SetIntervalMin(uint32_t minutes)
{
    if (minutes < 1U)
    {
        minutes = 1U;
    }
    if (minutes > DL_MAX_INTERVAL_MIN)
    {
        minutes = DL_MAX_INTERVAL_MIN;
    }
    s_interval_min = minutes;
    RESS_LOG("LOGGER", "interval set to %lu min", (unsigned long)s_interval_min);
}

uint32_t DataLogger_GetIntervalMin(void)
{
    return s_interval_min;
}

void DataLogger_UpdateFatTime(const DS3231_Time_t *now)
{
    if (now == NULL || now->year < 1980U)
    {
        return;
    }
    s_fattime = ((uint32_t)(now->year - 1980U) << 25) | ((uint32_t)now->month << 21) |
                ((uint32_t)now->date << 16) | ((uint32_t)now->hour << 11) |
                ((uint32_t)now->minute << 5) | ((uint32_t)now->second >> 1);
}

uint32_t DataLogger_GetFatTime(void)
{
    return s_fattime;
}

static bool EnsureMounted(void)
{
    if (s_mounted)
    {
        return true;
    }

    uint32_t now = HAL_GetTick();
    if (s_mount_tried && (now - s_last_mount_try) < DL_REMOUNT_RETRY_MS)
    {
        return false;
    }
    s_mount_tried = true;
    s_last_mount_try = now;

    RESS_LOG("LOGGER", "mounting SD card...");
    HAL_IWDG_Refresh(&hiwdg);

    if (!SD_SPI_Init())
    {
        RESS_LOG("LOGGER", "SD_SPI_Init FAILED (no card?) -- retry in %u s",
                 (unsigned)(DL_REMOUNT_RETRY_MS / 1000U));
        return false;
    }
    HAL_IWDG_Refresh(&hiwdg);

    FRESULT fr = f_mount(&USERFatFS, USERPath, 1);
    if (fr != FR_OK)
    {
        RESS_LOG("LOGGER", "f_mount FAILED FRESULT=%d (card must be FAT32)", (int)fr);
        return false;
    }

    s_mounted = true;
    RESS_LOG("LOGGER", "SD mounted OK");
    return true;
}

static bool WriteRow(const DS3231_Time_t *t, const PowerMeter_Data_t *d)
{
    char dir[16];
    char path[40];
    char row[360];
    char fields[240];
    FIL  file;
    UINT bw = 0;
    FRESULT fr;

    snprintf(dir, sizeof(dir), "%sLOGS", USERPath);
    snprintf(path, sizeof(path), "%sLOGS/%04u%02u%02u.CSV", USERPath,
             (unsigned)t->year, (unsigned)t->month, (unsigned)t->date);

    PowerMeter_FormatFields(fields, sizeof(fields), d);
    int n = snprintf(row, sizeof(row), "%04u-%02u-%02u %02u:%02u:%02u,%s,%s\r\n",
                     (unsigned)t->year, (unsigned)t->month, (unsigned)t->date,
                     (unsigned)t->hour, (unsigned)t->minute, (unsigned)t->second,
                     RESS_DEVICE_ID, fields);
    if (n <= 0 || n >= (int)sizeof(row))
    {
        RESS_LOG("LOGGER", "row too long, dropped");
        return false;
    }

    HAL_IWDG_Refresh(&hiwdg);

    fr = f_mkdir(dir);
    if (fr != FR_OK && fr != FR_EXIST)
    {
        RESS_LOG("LOGGER", "f_mkdir(%s) FAILED FRESULT=%d", dir, (int)fr);
        return false;
    }

    fr = f_open(&file, path, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        RESS_LOG("LOGGER", "f_open(%s) FAILED FRESULT=%d", path, (int)fr);
        return false;
    }

    bool new_file = (f_size(&file) == 0U);

    fr = f_lseek(&file, f_size(&file));
    if (fr == FR_OK && new_file)
    {
        fr = f_write(&file, CSV_HEADER, (UINT)strlen(CSV_HEADER), &bw);
        if (fr == FR_OK)
        {
            RESS_LOG("LOGGER", "new file %s, header written", path);
        }
    }
    HAL_IWDG_Refresh(&hiwdg);

    if (fr == FR_OK)
    {
        fr = f_write(&file, row, (UINT)n, &bw);
        if (fr == FR_OK && bw != (UINT)n)
        {
            fr = FR_DISK_ERR; /* disk full / short write */
        }
    }
    if (fr == FR_OK)
    {
        fr = f_sync(&file);
    }

    FRESULT fc = f_close(&file);
    HAL_IWDG_Refresh(&hiwdg);

    if (fr != FR_OK || fc != FR_OK)
    {
        RESS_LOG("LOGGER", "write FAILED FRESULT write/sync=%d close=%d", (int)fr, (int)fc);
        return false;
    }

    s_rows_written++;
    RESS_LOG("LOGGER", "row #%lu -> %s (%d bytes)", (unsigned long)s_rows_written, path, n);
    return true;
}

bool DataLogger_LogNow(const DS3231_Time_t *now, bool time_valid,
                       const PowerMeter_Data_t *data, bool meter_online)
{
    if (!time_valid)
    {
        RESS_LOG("LOGGER", "skip: RTC time not valid (use SETTIME)");
        return false;
    }
    if (!meter_online || !data->valid)
    {
        RESS_LOG("LOGGER", "skip: meter offline / no data");
        return false;
    }
    if (!EnsureMounted())
    {
        return false;
    }

    if (!WriteRow(now, data))
    {
        /* Force a fresh card init + mount on the next attempt. */
        s_mounted = false;
        s_mount_tried = false;
        (void)f_mount(NULL, USERPath, 0);
        return false;
    }
    return true;
}

void DataLogger_Process(const DS3231_Time_t *now, bool time_valid,
                        const PowerMeter_Data_t *data, bool meter_online)
{
    uint32_t tick = HAL_GetTick();
    uint32_t interval_ms = s_interval_min * 60000UL;

    if (s_logged_once && (tick - s_last_log_tick) < interval_ms)
    {
        return;
    }

    if (!time_valid || !meter_online)
    {
        /* Do not spam: report the blocker once every 30 s. */
        if ((tick - s_last_skip_print) >= 30000U)
        {
            s_last_skip_print = tick;
            RESS_LOG("LOGGER", "waiting: time_valid=%d meter_online=%d", (int)time_valid,
                     (int)meter_online);
        }
        return;
    }

    if (DataLogger_LogNow(now, time_valid, data, meter_online))
    {
        s_logged_once = true;
        s_last_log_tick = tick;
    }
    else
    {
        /* Back off one second so a failing card does not stall the loop. */
        s_logged_once = true;
        s_last_log_tick = tick - interval_ms + 1000U;
    }
}
