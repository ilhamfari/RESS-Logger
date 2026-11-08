#include "datalogger.h"
#include "fatfs.h"
#include "iwdg.h"
#include "sd_spi.h"
#include "ress_util.h"
#include "ress_config.h"
#include <string.h>

#define CSV_HEADER \
    "timestamp,seq," \
    "ua,ub,uc,uab,ubc,uca,ia,ib,ic,pa,pb,pc,p_total,qa,qb,qc,q_total,sa,sb,sc,s_total,pf1,pf2,pf3,pf_avg,freq," \
    "gw_vpv,gw_ipv,gw_ppv,gw_e_total,gw_p_out,gw_p_load,gw_status,gw_temp," \
    "sg_vpv,sg_ipv,sg_ppv,sg_e_total,sg_p_out,sg_p_load,sg_status,sg_temp," \
    "r1,r2,r3,r4,sent\r\n"

#define METER_FIELD_COUNT 26U

typedef struct
{
    uint32_t seq;
    char     path[28];
    uint32_t flag_offset; /* byte offset of the '0' SENT character */
    bool     used;
} Pending_t;

static bool      s_mounted;
static uint32_t  s_last_mount_try;
static bool      s_mount_tried;
static uint32_t  s_fattime;
static uint32_t  s_rows_written;
static uint32_t  s_rows_sent;
static Pending_t s_pending[DL_PENDING_MAX];
static uint8_t   s_pending_next;

void DataLogger_Init(void)
{
    s_mounted = false;
    s_mount_tried = false;
    s_rows_written = 0;
    s_rows_sent = 0;
    memset(s_pending, 0, sizeof(s_pending));
    RESS_LOG("LOGGER", "init: one row per $REQ,TELE from ESP32, dir=%sLOGS, file=YYYYMMDD.CSV", USERPath);
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

static void AppendNaFields(char *buf, size_t size, size_t *pos, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        int n = snprintf(&buf[*pos], size - *pos, i == 0U ? "NA" : ",NA");
        if (n > 0)
        {
            *pos += (size_t)n;
        }
    }
}

/* Builds the CSV row; returns length, sets *flag_idx to the SENT char index. */
static int BuildRow(char *row, size_t size, const RessSnapshot_t *s, size_t *flag_idx)
{
    const DS3231_Time_t *t = &s->time;
    char   fields[240];
    size_t pos = 0;
    int    n;

    n = snprintf(row, size, "%04u-%02u-%02u %02u:%02u:%02u,%lu,", (unsigned)t->year, (unsigned)t->month,
                 (unsigned)t->date, (unsigned)t->hour, (unsigned)t->minute, (unsigned)t->second,
                 (unsigned long)s->seq);
    if (n <= 0 || (size_t)n >= size)
    {
        return -1;
    }
    pos = (size_t)n;

    if (s->meter_online && s->meter.valid)
    {
        PowerMeter_FormatFields(fields, sizeof(fields), &s->meter);
        n = snprintf(&row[pos], size - pos, "%s", fields);
        if (n > 0)
        {
            pos += (size_t)n;
        }
    }
    else
    {
        AppendNaFields(row, size, &pos, METER_FIELD_COUNT);
    }

    Inverter_FormatFields(fields, sizeof(fields), &s->goodwe);
    n = snprintf(&row[pos], size - pos, ",%s", fields);
    if (n > 0)
    {
        pos += (size_t)n;
    }

    Inverter_FormatFields(fields, sizeof(fields), &s->sungrow);
    n = snprintf(&row[pos], size - pos, ",%s", fields);
    if (n > 0)
    {
        pos += (size_t)n;
    }

    n = snprintf(&row[pos], size - pos, ",%u,%u,%u,%u,", (unsigned)(s->relays & 1U),
                 (unsigned)((s->relays >> 1) & 1U), (unsigned)((s->relays >> 2) & 1U),
                 (unsigned)((s->relays >> 3) & 1U));
    if (n > 0)
    {
        pos += (size_t)n;
    }

    if (pos + 4U >= size)
    {
        return -1;
    }
    *flag_idx = pos;
    row[pos++] = '0'; /* SENT */
    row[pos++] = '\r';
    row[pos++] = '\n';
    row[pos] = '\0';
    return (int)pos;
}

static void PushPending(uint32_t seq, const char *path, uint32_t flag_offset)
{
    Pending_t *p = &s_pending[s_pending_next];
    s_pending_next = (uint8_t)((s_pending_next + 1U) % DL_PENDING_MAX);

    p->seq = seq;
    p->flag_offset = flag_offset;
    p->used = true;
    snprintf(p->path, sizeof(p->path), "%s", path);
}

static bool WriteRow(const RessSnapshot_t *s)
{
    char dir[16];
    char path[28];
    char row[560];
    FIL  file;
    UINT bw = 0;
    size_t flag_idx = 0;
    FRESULT fr;

    snprintf(dir, sizeof(dir), "%sLOGS", USERPath);
    snprintf(path, sizeof(path), "%sLOGS/%04u%02u%02u.CSV", USERPath, (unsigned)s->time.year,
             (unsigned)s->time.month, (unsigned)s->time.date);

    int n = BuildRow(row, sizeof(row), s, &flag_idx);
    if (n <= 0)
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

    fr = f_lseek(&file, f_size(&file));
    if (fr == FR_OK && f_size(&file) == 0U)
    {
        fr = f_write(&file, CSV_HEADER, (UINT)strlen(CSV_HEADER), &bw);
        if (fr == FR_OK)
        {
            RESS_LOG("LOGGER", "new file %s, header written", path);
        }
    }
    HAL_IWDG_Refresh(&hiwdg);

    uint32_t row_offset = (uint32_t)f_tell(&file);

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

    PushPending(s->seq, path, row_offset + (uint32_t)flag_idx);
    s_rows_written++;
    RESS_LOG("LOGGER", "DATA SAVED TO SD CARD (seq=%lu, file=%s, rows=%lu)", (unsigned long)s->seq, path,
             (unsigned long)s_rows_written);
    return true;
}

bool DataLogger_WriteSnapshot(const RessSnapshot_t *snap)
{
    if (!snap->time_valid)
    {
        RESS_LOG("LOGGER", "skip: RTC time not valid (use SETTIME)");
        return false;
    }
    if (!EnsureMounted())
    {
        return false;
    }

    if (!WriteRow(snap))
    {
        /* Force a fresh card init + mount on the next attempt. */
        s_mounted = false;
        s_mount_tried = false;
        (void)f_mount(NULL, USERPath, 0);
        return false;
    }
    return true;
}

bool DataLogger_MarkSent(uint32_t seq)
{
    for (uint8_t i = 0; i < DL_PENDING_MAX; i++)
    {
        Pending_t *p = &s_pending[i];
        if (!p->used || p->seq != seq)
        {
            continue;
        }

        if (!s_mounted)
        {
            RESS_LOG("LOGGER", "cannot mark seq=%lu sent: SD not mounted", (unsigned long)seq);
            return false;
        }

        FIL file;
        UINT bw = 0;
        HAL_IWDG_Refresh(&hiwdg);

        FRESULT fr = f_open(&file, p->path, FA_OPEN_EXISTING | FA_WRITE);
        if (fr == FR_OK)
        {
            fr = f_lseek(&file, p->flag_offset);
        }
        if (fr == FR_OK)
        {
            fr = f_write(&file, "1", 1, &bw);
        }
        if (fr == FR_OK)
        {
            fr = f_sync(&file);
        }
        FRESULT fc = f_close(&file);
        HAL_IWDG_Refresh(&hiwdg);

        if (fr != FR_OK || fc != FR_OK || bw != 1U)
        {
            RESS_LOG("LOGGER", "mark sent seq=%lu FAILED FRESULT=%d/%d", (unsigned long)seq, (int)fr, (int)fc);
            return false;
        }

        p->used = false;
        s_rows_sent++;
        RESS_LOG("LOGGER", "seq=%lu marked SENT=1 in %s", (unsigned long)seq, p->path);
        return true;
    }

    RESS_LOG("LOGGER", "seq=%lu not in pending list (not logged or too old)", (unsigned long)seq);
    return false;
}

uint32_t DataLogger_GetRowsWritten(void)
{
    return s_rows_written;
}

uint32_t DataLogger_GetRowsSent(void)
{
    return s_rows_sent;
}

uint8_t DataLogger_GetPendingCount(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < DL_PENDING_MAX; i++)
    {
        if (s_pending[i].used)
        {
            n++;
        }
    }
    return n;
}

bool DataLogger_IsMounted(void)
{
    return s_mounted;
}
