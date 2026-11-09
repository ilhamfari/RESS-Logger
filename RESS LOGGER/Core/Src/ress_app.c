#include "ress_app.h"
#include "ress_config.h"
#include "ress_util.h"
#include "ress_snapshot.h"
#include "modbus_master.h"
#include "power_meter.h"
#include "inverters.h"
#include "relay.h"
#include "datalogger.h"
#include "esp_bridge.h"
#include "usart.h"
#include <string.h>

static DS3231_Time_t   s_now;
static bool            s_time_valid;
static uint32_t        s_last_rtc_tick;
static uint32_t        s_last_rtc_warn_tick;
static RessTimeSource_t s_time_src;
static uint32_t        s_sync_tick;
static uint32_t        s_last_time_req;

static RessSnapshot_t  s_snap;      /* most recent snapshot */
static RessSnapshot_t  s_replay_snap;
static uint32_t        s_last_seq;

#define UNIX_EPOCH_2026_UTC  1767225600UL

/* ------------------------------------------------------------------------- */
/* Time                                                                       */
/* ------------------------------------------------------------------------- */

static void IncrementSecond(DS3231_Time_t *t)
{
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    t->second++;
    if (t->second < 60U) return;
    t->second = 0;
    t->minute++;
    if (t->minute < 60U) return;
    t->minute = 0;
    t->hour++;
    if (t->hour < 24U) return;
    t->hour = 0;
    t->date++;

    uint8_t dmax = dim[t->month - 1U];
    if (t->month == 2U && ((t->year % 4U) == 0U && ((t->year % 100U) != 0U || (t->year % 400U) == 0U)))
    {
        dmax = 29U;
    }
    if (t->date <= dmax) return;
    t->date = 1U;
    t->month++;
    if (t->month <= 12U) return;
    t->month = 1U;
    t->year++;
}

/* Software clock: the time is set by the ESP32 (NTP) or the console and then
 * advanced from HAL_GetTick(). The I2C bus / DS3231 is not used at all. */
static void RefreshTime(void)
{
    uint32_t tick = HAL_GetTick();
    uint32_t dt = tick - s_last_rtc_tick;
    if (dt < RESS_RTC_REFRESH_MS)
    {
        return;
    }

    uint32_t elapsed_s = dt / 1000U;
    s_last_rtc_tick += elapsed_s * 1000U; /* keep the sub-second remainder: no drift */

    if (s_time_valid)
    {
        while (elapsed_s-- > 0U)
        {
            IncrementSecond(&s_now);
        }
        DataLogger_UpdateFatTime(&s_now);
    }
    else if ((tick - s_last_rtc_warn_tick) >= 30000U)
    {
        s_last_rtc_warn_tick = tick;
        RESS_LOG("TIME", "NOT valid, waiting for NTP from ESP32 ($CMD,TIME) or SETTIME");
    }
}

static void ForceTimeRefresh(void)
{
    RefreshTime(); /* catches up all whole seconds elapsed so far */
}

/* Seconds since 2026-01-01 00:00:00; 0 if before that. */
static uint32_t SecondsSince2026(const DS3231_Time_t *t)
{
    static const uint16_t cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    if (t->year < 2026U || t->month < 1U || t->month > 12U)
    {
        return 0;
    }

    uint32_t days = 0;
    for (uint16_t y = 2026U; y < t->year; y++)
    {
        days += ((y % 4U) == 0U) ? 366U : 365U;
    }
    days += cum[t->month - 1U];
    if (t->month > 2U && (t->year % 4U) == 0U)
    {
        days += 1U;
    }
    days += (uint32_t)(t->date - 1U);

    return days * 86400UL + (uint32_t)t->hour * 3600UL + (uint32_t)t->minute * 60UL + t->second;
}

/* Convert a UTC Unix timestamp to the plain date/time structure used by the
 * logger.  This lets a delayed main-loop task retain its scheduled minute. */
static bool EpochToDateTime(uint32_t epoch, DS3231_Time_t *out)
{
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (out == NULL || epoch < UNIX_EPOCH_2026_UTC)
    {
        return false;
    }

    uint32_t days = epoch / 86400UL;
    uint32_t sod = epoch % 86400UL;
    uint16_t year = 1970U;

    for (;;)
    {
        uint16_t diy = ((year % 4U) == 0U && ((year % 100U) != 0U || (year % 400U) == 0U)) ? 366U : 365U;
        if (days < diy) break;
        days -= diy;
        year++;
    }

    uint8_t month = 1U;
    while (month <= 12U)
    {
        uint8_t mdays = dim[month - 1U];
        if (month == 2U && ((year % 4U) == 0U && ((year % 100U) != 0U || (year % 400U) == 0U)))
        {
            mdays = 29U;
        }
        if (days < mdays) break;
        days -= mdays;
        month++;
    }

    *out = (DS3231_Time_t){
        year, month, (uint8_t)(days + 1U),
        (uint8_t)(sod / 3600UL),
        (uint8_t)((sod % 3600UL) / 60UL),
        (uint8_t)(sod % 60UL)
    };
    return true;
}

/* ------------------------------------------------------------------------- */
/* Snapshot                                                                   */
/* ------------------------------------------------------------------------- */

/* Sequence = seconds since 2026-01-01 (unique across reboots), forced to be
 * strictly increasing even if the RTC is invalid or two snapshots share a
 * second. */
static uint32_t NextSeq(const DS3231_Time_t *timestamp)
{
    uint32_t seq = s_time_valid ? SecondsSince2026(timestamp) : 0U;
    if (seq <= s_last_seq)
    {
        seq = s_last_seq + 1U;
    }
    s_last_seq = seq;
    return seq;
}

static void TakeSnapshot(uint32_t scheduled_epoch, int16_t rssi, uint32_t uptime,
                         const char *fw_version)
{
    RessSnapshot_t *s = &s_snap;
    memset(s, 0, sizeof(*s));

    ForceTimeRefresh();
    s->valid = true;
    s->time = s_now;
    (void)EpochToDateTime(scheduled_epoch, &s->time);
    /* CSV and MQTT timestamps are minute buckets, even when execution of the
     * queued task happens several seconds late. */
    s->time.second = 0U;
    if (scheduled_epoch == 0U && s_time_valid)
    {
        scheduled_epoch = UNIX_EPOCH_2026_UTC + SecondsSince2026(&s->time);
    }
    s->seq = NextSeq(&s->time);
    s->epoch = scheduled_epoch;
    s->time_valid = s_time_valid;
    GoodWe_GetLatest(&s->goodwe);
    s->relays = Relay_GetMask();
    if (fw_version != NULL && fw_version[0] != '\0')
    {
        s->gateway_health_valid = true;
        s->gateway_rssi = rssi;
        s->gateway_uptime = uptime;
        snprintf(s->gateway_fw_version, sizeof(s->gateway_fw_version), "%s", fw_version);
    }
    s->sent = false;

    RESS_LOG("APP", "snapshot seq=%lu goodwe=%d relays=0x%X", (unsigned long)s->seq,
             (int)s->goodwe.online, (unsigned)s->relays);
}

/* Take a snapshot and log it to SD. Offline devices are written as NA, so a
 * row is written whenever the clock is valid. */
static bool SnapshotAndLog(uint32_t scheduled_epoch, int16_t rssi, uint32_t uptime,
                           const char *fw_version)
{
    TakeSnapshot(scheduled_epoch, rssi, uptime, fw_version);
    return DataLogger_WriteSnapshot(&s_snap);
}

/* ------------------------------------------------------------------------- */
/* ESP32 bridge handlers                                                      */
/* ------------------------------------------------------------------------- */

static void OnReqTele(uint32_t scheduled_epoch, int16_t rssi, uint32_t uptime,
                      const char *fw_version)
{
    if (!GoodWe_IsEnabled())
    {
        RESS_LOG("APP", "telemetry request ignored: GoodWe acquisition disabled");
        return;
    }

    /* The ESP32 poll interval (Telemetry Send Interval) is the log interval:
     * every request = one fresh snapshot = one SD row = one telemetry batch. */
    if (SnapshotAndLog(scheduled_epoch, rssi, uptime, fw_version))
    {
        (void)ESP_Bridge_SendSnapshot(&s_snap);
    }
    else
    {
        RESS_LOG("APP", "telemetry not sent because durable SD write failed");
    }
}

static void OnReqPending(void)
{
    if (DataLogger_LoadOldestPending(&s_replay_snap))
    {
        RESS_LOG("APP", "replaying oldest pending seq=%lu", (unsigned long)s_replay_snap.seq);
        (void)ESP_Bridge_SendSnapshot(&s_replay_snap);
    }
    else
    {
        (void)ESP_Bridge_SendPendingNone();
    }
}

static int OnCmdRelay(uint8_t channel, uint8_t state)
{
    if (!Relay_SetAndSave(channel, state != 0U))
    {
        return -1;
    }
    return Relay_Get(channel) ? 1 : 0;
}

static void OnCmdGoodWe(bool enabled)
{
    GoodWe_SetEnabled(enabled);
}

static void OnAckTele(uint32_t seq, bool success)
{
    if (!success)
    {
        RESS_LOG("APP", "ESP32 reported publish FAILED for seq=%lu", (unsigned long)seq);
        return;
    }

    if (s_snap.valid && s_snap.seq == seq)
    {
        s_snap.sent = true;
    }
    (void)DataLogger_MarkSent(seq);
}

static bool OnCmdTime(uint16_t year, uint8_t month, uint8_t date,
                      uint8_t hour, uint8_t minute, uint8_t second, uint32_t epoch)
{
    RESS_LOG("APP", "Processing NTP sync request: %04u-%02u-%02u %02u:%02u:%02u (epoch=%lu)",
             (unsigned)year, (unsigned)month, (unsigned)date,
             (unsigned)hour, (unsigned)minute, (unsigned)second, (unsigned long)epoch);
    bool ok = RESS_App_SetTime(year, month, date, hour, minute, second);
    if (ok)
    {
        s_time_src = TIME_SRC_NTP;
        RESS_LOG("APP", ">>> NTP TIME IS NOW VALID! SD Logger timestamp unlocked <<<");
    }
    else
    {
        RESS_LOG("APP", ">>> NTP TIME SYNC FAILED <<<");
    }
    return ok;
}

static const ESP_Handlers_t s_handlers =
{
    .on_req_tele = OnReqTele,
    .on_req_pending = OnReqPending,
    .on_cmd_relay = OnCmdRelay,
    .on_cmd_goodwe = OnCmdGoodWe,
    .on_ack_tele = OnAckTele,
    .on_cmd_time = OnCmdTime,
};

/* ------------------------------------------------------------------------- */
/* Public                                                                     */
/* ------------------------------------------------------------------------- */

void RESS_App_Init(void)
{
    printf("\r\n");
    RESS_LOG("APP", "==== RESS V logger start, device=%s ====", RESS_DEVICE_ID);

    s_last_rtc_tick = HAL_GetTick();
    RESS_LOG("TIME", "software clock (no I2C/DS3231), waiting for NTP from ESP32");

    Relay_Init();

    Modbus_Init(&huart1);
    GoodWe_Init(RESS_GOODWE_SLAVE_ID, RESS_GOODWE_POLL_MS);

    DataLogger_Init();
    ESP_Bridge_Init(&huart3, &s_handlers);
    s_last_time_req = HAL_GetTick() - 5000U; /* ask for the time right away */

    RESS_LOG("APP", "ready. Console commands: HELP");
}

void RESS_App_Process(void)
{
    Modbus_Poll();
    RefreshTime();
    ESP_Bridge_Process();

    /* No valid time yet: keep asking the ESP32 (it answers with $CMD,TIME). */
    uint32_t tick = HAL_GetTick();
    if (!s_time_valid && (tick - s_last_time_req) >= 5000U)
    {
        s_last_time_req = tick;
        (void)ESP_Bridge_SendReqTime();
    }
}

void RESS_App_PrintMeter(void)
{
    PowerMeter_Data_t data;
    (void)PowerMeter_GetLatest(&data);
    PowerMeter_PrintDebug(&data);
}

void RESS_App_PrintInverters(void)
{
    GoodWe_Data_t d;
    GoodWe_GetLatest(&d);
    GoodWe_PrintDebug(&d);
    RESS_LOG("RELAY", "mask=0x%X", (unsigned)Relay_GetMask());
}

void RESS_App_LogNow(void)
{
    (void)SnapshotAndLog(0U, 0, 0U, NULL);
}

bool RESS_App_SetRelay(uint8_t channel, bool on)
{
    return Relay_SetAndSave(channel, on);
}

bool RESS_App_SetTime(uint16_t year, uint8_t month, uint8_t date,
                      uint8_t hour, uint8_t minute, uint8_t second)
{
    if (year < 2000U || year > 2099U || month < 1U || month > 12U || date < 1U || date > 31U ||
        hour > 23U || minute > 59U || second > 59U)
    {
        RESS_LOG("RTC", "SETTIME rejected: value out of range");
        return false;
    }

    s_now = (DS3231_Time_t){ year, month, date, hour, minute, second };
    s_time_valid = true;
    s_last_rtc_tick = HAL_GetTick();
    s_sync_tick = s_last_rtc_tick;
    s_time_src = TIME_SRC_MANUAL; /* OnCmdTime() overrides this to NTP */
    DataLogger_UpdateFatTime(&s_now);

    RESS_LOG("TIME", "software clock set: %04u-%02u-%02u %02u:%02u:%02u UTC", (unsigned)year,
             (unsigned)month, (unsigned)date, (unsigned)hour, (unsigned)minute, (unsigned)second);
    return true;
}

void RESS_App_GetDeviceStatus(bool *meter_on, bool *goodwe_on, bool *sungrow_on)
{
    GoodWe_Data_t g;

    GoodWe_GetLatest(&g);

    if (meter_on != NULL)   { *meter_on = false; }
    if (goodwe_on != NULL)  { *goodwe_on = g.online; }
    if (sungrow_on != NULL) { *sungrow_on = false; }
}

bool RESS_App_GetTimeInfo(DS3231_Time_t *out_time, bool *is_valid)
{
    RefreshTime();
    if (out_time != NULL)
    {
        *out_time = s_now;
    }
    if (is_valid != NULL)
    {
        *is_valid = s_time_valid;
    }
    return s_time_valid;
}

RessTimeSource_t RESS_App_GetTimeSource(uint32_t *age_s)
{
    if (age_s != NULL)
    {
        *age_s = (s_time_src == TIME_SRC_NONE) ? 0U : (HAL_GetTick() - s_sync_tick) / 1000U;
    }
    return s_time_src;
}
