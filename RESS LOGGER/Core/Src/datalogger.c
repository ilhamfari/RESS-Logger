#include "datalogger.h"
#include "fatfs.h"
#include "iwdg.h"
#include "sd_spi.h"
#include "ress_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DL_LOG_DIR "GWLOG2"
#define DL_QUEUE_DIR "GWPEND"
#define DL_ROW_SIZE 2048U
#define DL_COLUMN_COUNT 45U

#define CSV_HEADER \
 "timestamp,epoch,seq,device_serial,device_type,error_code,operating_hours_total,working_status," \
 "grid_connection_status,inverter_temperature,mppt1_voltage,mppt1_current,mppt1_power," \
 "mppt2_voltage,mppt2_current,mppt2_power,pv_power_total,insulation_resistance,leakage_current," \
 "active_power,reactive_power,power_factor,phase_a_voltage,phase_b_voltage,phase_c_voltage," \
 "phase_a_current,phase_b_current,phase_c_current,phase_a_frequency,phase_b_frequency," \
 "phase_c_frequency,grid_frequency,line_ab_voltage,line_bc_voltage,line_ca_voltage," \
 "energy_total,energy_today,gateway_rssi,gateway_uptime,gateway_fw_version,r1,r2,r3,r4,sent\r\n"

static bool s_mounted, s_mount_tried;
static uint32_t s_last_mount_try, s_fattime, s_rows_written, s_rows_sent;
static char s_row[DL_ROW_SIZE];

void DataLogger_Init(void)
{
    s_mounted = s_mount_tried = false;
    s_rows_written = s_rows_sent = 0U;
    RESS_LOG("LOGGER", "full GoodWe CSV=%s%s, durable queue=%s%s", USERPath, DL_LOG_DIR,
             USERPath, DL_QUEUE_DIR);
}

void DataLogger_UpdateFatTime(const DS3231_Time_t *t)
{
    if (t == NULL || t->year < 1980U) return;
    s_fattime = ((uint32_t)(t->year - 1980U) << 25) | ((uint32_t)t->month << 21) |
                ((uint32_t)t->date << 16) | ((uint32_t)t->hour << 11) |
                ((uint32_t)t->minute << 5) | ((uint32_t)t->second >> 1);
}

uint32_t DataLogger_GetFatTime(void) { return s_fattime; }

static bool EnsureMounted(void)
{
    if (s_mounted) return true;
    uint32_t now = HAL_GetTick();
    if (s_mount_tried && now - s_last_mount_try < DL_REMOUNT_RETRY_MS) return false;
    s_mount_tried = true;
    s_last_mount_try = now;
    HAL_IWDG_Refresh(&hiwdg);
    if (!SD_SPI_Init() || f_mount(&USERFatFS, USERPath, 1) != FR_OK)
    {
        RESS_LOG("LOGGER", "SD mount failed; retry later");
        return false;
    }
    s_mounted = true;
    RESS_LOG("LOGGER", "SD mounted");
    return true;
}

static void InvalidateMount(void)
{
    s_mounted = s_mount_tried = false;
    (void)f_mount(NULL, USERPath, 0);
}

static bool EnsureDirectories(void)
{
    char p[20];
    snprintf(p, sizeof(p), "%s%s", USERPath, DL_LOG_DIR);
    FRESULT fr = f_mkdir(p);
    if (fr != FR_OK && fr != FR_EXIST) return false;
    snprintf(p, sizeof(p), "%s%s", USERPath, DL_QUEUE_DIR);
    fr = f_mkdir(p);
    return fr == FR_OK || fr == FR_EXIST;
}

static int BuildRow(char *row, size_t cap, const RessSnapshot_t *s, size_t *sent_index)
{
    char fields[GOODWE_FIELD_COUNT * 25U];
    char rssi[16] = "NA", uptime[16] = "NA";
    const char *fw = "NA";
    if (GoodWe_FormatFields(fields, sizeof(fields), &s->goodwe) < 0) return -1;
    if (s->gateway_health_valid)
    {
        snprintf(rssi, sizeof(rssi), "%d", (int)s->gateway_rssi);
        snprintf(uptime, sizeof(uptime), "%lu", (unsigned long)s->gateway_uptime);
        if (s->gateway_fw_version[0] != '\0') fw = s->gateway_fw_version;
    }
    const DS3231_Time_t *t = &s->time;
    int n = snprintf(row, cap,
        "%04u-%02u-%02u %02u:%02u:%02u,%lu,%lu,%s,%s,%s,%s,%u,%u,%u,%u,",
        (unsigned)t->year, (unsigned)t->month, (unsigned)t->date, (unsigned)t->hour,
        (unsigned)t->minute, (unsigned)t->second, (unsigned long)s->epoch,
        (unsigned long)s->seq, fields, rssi, uptime, fw,
        (unsigned)(s->relays & 1U), (unsigned)((s->relays >> 1) & 1U),
        (unsigned)((s->relays >> 2) & 1U), (unsigned)((s->relays >> 3) & 1U));
    if (n <= 0 || (size_t)n + 4U >= cap) return -1;
    *sent_index = (size_t)n;
    row[n++] = '0'; row[n++] = '\r'; row[n++] = '\n'; row[n] = '\0';
    return n;
}

static bool WriteQueue(uint32_t seq, const char *csv_path, uint32_t offset,
                       const char *row, size_t row_len)
{
    char path[32], header[64];
    snprintf(path, sizeof(path), "%s%s/%08lX.BUF", USERPath, DL_QUEUE_DIR,
             (unsigned long)seq);
    int hn = snprintf(header, sizeof(header), "%s|%lu\n", csv_path, (unsigned long)offset);
    if (hn <= 0 || (size_t)hn >= sizeof(header)) return false;
    FIL f; UINT bw = 0U;
    FRESULT fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    bool opened = fr == FR_OK;
    if (fr == FR_OK) { fr = f_write(&f, header, (UINT)hn, &bw); if (bw != (UINT)hn) fr = FR_DISK_ERR; }
    if (fr == FR_OK) { fr = f_write(&f, row, (UINT)row_len, &bw); if (bw != (UINT)row_len) fr = FR_DISK_ERR; }
    if (fr == FR_OK) fr = f_sync(&f);
    FRESULT fc = opened ? f_close(&f) : fr;
    return fr == FR_OK && fc == FR_OK;
}

bool DataLogger_WriteSnapshot(const RessSnapshot_t *s)
{
    if (s == NULL || !s->time_valid || !EnsureMounted() || !EnsureDirectories()) return false;
    size_t sent_index;
    int row_len = BuildRow(s_row, sizeof(s_row), s, &sent_index);
    if (row_len <= 0) { RESS_LOG("LOGGER", "CSV row overflow"); return false; }
    char path[32];
    snprintf(path, sizeof(path), "%s%s/%04u%02u%02u.CSV", USERPath, DL_LOG_DIR,
             (unsigned)s->time.year, (unsigned)s->time.month, (unsigned)s->time.date);
    FIL f; UINT bw = 0U;
    FRESULT fr = f_open(&f, path, FA_OPEN_ALWAYS | FA_WRITE);
    bool opened = fr == FR_OK;
    if (fr == FR_OK) fr = f_lseek(&f, f_size(&f));
    if (fr == FR_OK && f_size(&f) == 0U)
    {
        fr = f_write(&f, CSV_HEADER, (UINT)strlen(CSV_HEADER), &bw);
        if (bw != (UINT)strlen(CSV_HEADER)) fr = FR_DISK_ERR;
    }
    uint32_t row_offset = (fr == FR_OK) ? (uint32_t)f_tell(&f) : 0U;
    if (fr == FR_OK) { fr = f_write(&f, s_row, (UINT)row_len, &bw); if (bw != (UINT)row_len) fr = FR_DISK_ERR; }
    if (fr == FR_OK) fr = f_sync(&f);
    FRESULT fc = opened ? f_close(&f) : fr;
    if (fr != FR_OK || fc != FR_OK ||
        !WriteQueue(s->seq, path, row_offset + (uint32_t)sent_index, s_row, (size_t)row_len))
    {
        RESS_LOG("LOGGER", "write/queue failed seq=%lu", (unsigned long)s->seq);
        InvalidateMount();
        return false;
    }
    s_rows_written++;
    RESS_LOG("LOGGER", "saved seq=%lu", (unsigned long)s->seq);
    return true;
}

static bool ReadQueueHeader(FIL *f, char *csv, size_t cap, uint32_t *offset)
{
    char h[64];
    if (f_gets(h, (int)sizeof(h), f) == NULL) return false;
    char *sep = strchr(h, '|');
    if (sep == NULL) return false;
    *sep++ = '\0';
    char *nl = strchr(sep, '\n'); if (nl != NULL) *nl = '\0';
    if (csv != NULL && cap > 0U) snprintf(csv, cap, "%s", h);
    if (offset != NULL) *offset = (uint32_t)strtoul(sep, NULL, 10);
    return true;
}

bool DataLogger_MarkSent(uint32_t seq)
{
    if (!EnsureMounted()) return false;
    char qp[32], cp[32]; uint32_t offset;
    snprintf(qp, sizeof(qp), "%s%s/%08lX.BUF", USERPath, DL_QUEUE_DIR, (unsigned long)seq);
    FIL q; FRESULT fr = f_open(&q, qp, FA_READ);
    bool ok = fr == FR_OK && ReadQueueHeader(&q, cp, sizeof(cp), &offset);
    if (fr == FR_OK) (void)f_close(&q);
    if (!ok) return false;
    FIL f; UINT bw = 0U;
    fr = f_open(&f, cp, FA_OPEN_EXISTING | FA_WRITE);
    bool opened = fr == FR_OK;
    if (fr == FR_OK) fr = f_lseek(&f, offset);
    if (fr == FR_OK) fr = f_write(&f, "1", 1U, &bw);
    if (fr == FR_OK) fr = f_sync(&f);
    FRESULT fc = opened ? f_close(&f) : fr;
    if (fr != FR_OK || fc != FR_OK || bw != 1U || f_unlink(qp) != FR_OK) return false;
    s_rows_sent++;
    RESS_LOG("LOGGER", "acknowledged seq=%lu", (unsigned long)seq);
    return true;
}

static bool IsNA(const char *s) { return s == NULL || *s == '\0' || strcmp(s, "NA") == 0; }

static char *NextField(char **cursor)
{
    if (cursor == NULL || *cursor == NULL) return NULL;
    char *v = *cursor, *comma = strchr(v, ',');
    if (comma != NULL) { *comma = '\0'; *cursor = comma + 1; }
    else { char *e = v + strlen(v); while (e > v && (e[-1] == '\r' || e[-1] == '\n')) *--e = '\0'; *cursor = NULL; }
    return v;
}

static bool ParseRow(char *row, RessSnapshot_t *out)
{
    char *v[DL_COLUMN_COUNT], *c = row;
    for (size_t n = 0U; n < DL_COLUMN_COUNT; n++) { v[n] = NextField(&c); if (v[n] == NULL) return false; }
    memset(out, 0, sizeof(*out));
    unsigned yy, mo, dd, hh, mm, ss;
    if (sscanf(v[0], "%u-%u-%u %u:%u:%u", &yy, &mo, &dd, &hh, &mm, &ss) != 6) return false;
    out->time = (DS3231_Time_t){(uint16_t)yy,(uint8_t)mo,(uint8_t)dd,(uint8_t)hh,(uint8_t)mm,(uint8_t)ss};
    out->epoch = (uint32_t)strtoul(v[1], NULL, 10); out->seq = (uint32_t)strtoul(v[2], NULL, 10);
    out->valid = out->time_valid = out->replayed = true;
    GoodWe_Data_t *g = &out->goodwe; size_t i = 3U;
#define PT(flag,dst) do { if (!IsNA(v[i])) { (flag)=true; snprintf((dst),sizeof(dst),"%s",v[i]); } i++; } while(0)
#define PU(flag,dst) do { if (!IsNA(v[i])) { (flag)=true; (dst)=(uint32_t)strtoul(v[i],NULL,10); } i++; } while(0)
#define PF(flag,dst) do { if (!IsNA(v[i])) { (flag)=true; (dst)=strtof(v[i],NULL); } i++; } while(0)
    PT(g->has_device_serial,g->device_serial); PT(g->has_device_type,g->device_type);
    PU(g->has_error_code,g->error_code); PU(g->has_operating_hours,g->operating_hours_total);
    PT(g->has_working_status,g->working_status); PT(g->has_grid_connection_status,g->grid_connection_status);
    PF(g->has_inverter_temperature,g->inverter_temperature);
    PF(g->has_mppt1_voltage,g->mppt1_voltage); PF(g->has_mppt1_current,g->mppt1_current); PF(g->has_mppt1_power,g->mppt1_power);
    PF(g->has_mppt2_voltage,g->mppt2_voltage); PF(g->has_mppt2_current,g->mppt2_current); PF(g->has_mppt2_power,g->mppt2_power);
    PF(g->has_pv_power_total,g->pv_power_total); PF(g->has_insulation_resistance,g->insulation_resistance); PF(g->has_leakage_current,g->leakage_current);
    PF(g->has_active_power,g->active_power); PF(g->has_reactive_power,g->reactive_power); PF(g->has_power_factor,g->power_factor);
    PF(g->has_phase_a_voltage,g->phase_a_voltage); PF(g->has_phase_b_voltage,g->phase_b_voltage); PF(g->has_phase_c_voltage,g->phase_c_voltage);
    PF(g->has_phase_a_current,g->phase_a_current); PF(g->has_phase_b_current,g->phase_b_current); PF(g->has_phase_c_current,g->phase_c_current);
    PF(g->has_phase_a_frequency,g->phase_a_frequency); PF(g->has_phase_b_frequency,g->phase_b_frequency); PF(g->has_phase_c_frequency,g->phase_c_frequency);
    PF(g->has_grid_frequency,g->grid_frequency); PF(g->has_line_ab_voltage,g->line_ab_voltage); PF(g->has_line_bc_voltage,g->line_bc_voltage);
    PF(g->has_line_ca_voltage,g->line_ca_voltage); PF(g->has_energy_total,g->energy_total); PF(g->has_energy_today,g->energy_today);
#undef PT
#undef PU
#undef PF
    g->online = g->has_device_serial || g->has_error_code || g->has_active_power;
    if (!IsNA(v[i])) { out->gateway_health_valid=true; out->gateway_rssi=(int16_t)strtol(v[i],NULL,10); } i++;
    if (!IsNA(v[i])) { out->gateway_health_valid=true; out->gateway_uptime=(uint32_t)strtoul(v[i],NULL,10); } i++;
    if (!IsNA(v[i])) { out->gateway_health_valid=true; snprintf(out->gateway_fw_version,sizeof(out->gateway_fw_version),"%s",v[i]); } i++;
    for (uint8_t bit=0U; bit<4U; bit++,i++) if (atoi(v[i]) != 0) out->relays |= (uint8_t)(1U<<bit);
    return i == 44U;
}

bool DataLogger_LoadOldestPending(RessSnapshot_t *out)
{
    if (out == NULL || !EnsureMounted() || !EnsureDirectories()) return false;
    char dp[20], oldest[20] = ""; snprintf(dp, sizeof(dp), "%s%s", USERPath, DL_QUEUE_DIR);
    DIR d; FILINFO info; FRESULT fr = f_opendir(&d, dp); if (fr != FR_OK) return false;
    while (f_readdir(&d, &info) == FR_OK && info.fname[0] != '\0')
    {
        size_t n = strlen(info.fname);
        if (n == 12U && strcmp(&info.fname[8], ".BUF") == 0 &&
            (oldest[0] == '\0' || strcmp(info.fname, oldest) < 0)) snprintf(oldest,sizeof(oldest),"%s",info.fname);
    }
    (void)f_closedir(&d); if (oldest[0] == '\0') return false;
    char p[40]; snprintf(p,sizeof(p),"%s/%s",dp,oldest); FIL f; fr=f_open(&f,p,FA_READ); if(fr!=FR_OK)return false;
    bool ok = ReadQueueHeader(&f,NULL,0U,NULL) && f_gets(s_row,(int)sizeof(s_row),&f)!=NULL && ParseRow(s_row,out);
    (void)f_close(&f); if (!ok) RESS_LOG("LOGGER", "invalid queue record %s", p); return ok;
}

uint32_t DataLogger_GetRowsWritten(void) { return s_rows_written; }
uint32_t DataLogger_GetRowsSent(void) { return s_rows_sent; }
uint8_t DataLogger_GetPendingCount(void)
{
    if (!EnsureMounted()) return 0U;
    char p[20]; snprintf(p,sizeof(p),"%s%s",USERPath,DL_QUEUE_DIR); DIR d; FILINFO i; uint16_t n=0U;
    if(f_opendir(&d,p)!=FR_OK)return 0U;
    while(f_readdir(&d,&i)==FR_OK && i.fname[0]!='\0') { size_t l=strlen(i.fname); if(l==12U && strcmp(&i.fname[8],".BUF")==0 && n<255U)n++; }
    (void)f_closedir(&d); return (uint8_t)n;
}
bool DataLogger_IsMounted(void) { return s_mounted; }
