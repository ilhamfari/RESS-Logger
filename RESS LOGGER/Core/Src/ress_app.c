#include "ress_app.h"
#include "ress_config.h"
#include "ress_util.h"
#include "modbus_master.h"
#include "power_meter.h"
#include "datalogger.h"
#include "esp_bridge.h"
#include "ds3231.h"
#include "i2c.h"
#include "usart.h"
#include <string.h>

static DS3231_Handle_t s_rtc;
static DS3231_Time_t   s_now;
static bool            s_time_valid;
static uint32_t        s_last_rtc_tick;
static uint32_t        s_last_esp_tick;
static uint32_t        s_last_rtc_warn_tick;

static void FormatTimestamp(char *buf, size_t size)
{
    if (s_time_valid)
    {
        snprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u", (unsigned)s_now.year,
                 (unsigned)s_now.month, (unsigned)s_now.date, (unsigned)s_now.hour,
                 (unsigned)s_now.minute, (unsigned)s_now.second);
    }
    else
    {
        snprintf(buf, size, "0000-00-00 00:00:00");
    }
}

static void RefreshTime(void)
{
    uint32_t tick = HAL_GetTick();
    if ((tick - s_last_rtc_tick) < RESS_RTC_REFRESH_MS)
    {
        return;
    }
    s_last_rtc_tick = tick;

    if (DS3231_GetTime(&s_rtc, &s_now) == HAL_OK)
    {
        s_time_valid = true;
        DataLogger_UpdateFatTime(&s_now);
    }
    else
    {
        s_time_valid = false;
        if ((tick - s_last_rtc_warn_tick) >= 30000U)
        {
            s_last_rtc_warn_tick = tick;
            RESS_LOG("RTC", "time NOT valid (no response or oscillator stopped) -- "
                            "use: SETTIME YYYY-MM-DD HH:MM:SS");
        }
    }
}

void RESS_App_Init(void)
{
    printf("\r\n");
    RESS_LOG("APP", "==== RESS V logger start, device=%s ====", RESS_DEVICE_ID);

    DS3231_Init(&s_rtc, &hi2c1);
    s_last_rtc_tick = HAL_GetTick() - RESS_RTC_REFRESH_MS; /* force first read */
    RefreshTime();
    if (s_time_valid)
    {
        char ts[24];
        FormatTimestamp(ts, sizeof(ts));
        RESS_LOG("RTC", "time = %s", ts);
    }

    Modbus_Init(&huart1);
    PowerMeter_Init(RESS_METER_SLAVE_ID, RESS_METER_POLL_MS);
    DataLogger_Init();
    ESP_Bridge_Init(&huart3);

    s_last_esp_tick = HAL_GetTick();
    RESS_LOG("APP", "ready. Console commands: HELP");
}

void RESS_App_Process(void)
{
    PowerMeter_Data_t data;

    Modbus_Poll();
    RefreshTime();

    bool online = PowerMeter_GetLatest(&data);

    DataLogger_Process(&s_now, s_time_valid, &data, online);

    uint32_t tick = HAL_GetTick();
    if ((tick - s_last_esp_tick) >= RESS_ESP_TX_INTERVAL_MS)
    {
        s_last_esp_tick = tick;
        if (online)
        {
            char ts[24];
            FormatTimestamp(ts, sizeof(ts));
            (void)ESP_Bridge_SendTelemetry(ts, &data);
        }
        else
        {
            (void)ESP_Bridge_SendStatus(false);
        }
    }
}

void RESS_App_PrintMeter(void)
{
    PowerMeter_Data_t data;
    (void)PowerMeter_GetLatest(&data);
    PowerMeter_PrintDebug(&data);
}

void RESS_App_LogNow(void)
{
    PowerMeter_Data_t data;
    bool online = PowerMeter_GetLatest(&data);
    s_last_rtc_tick = HAL_GetTick() - RESS_RTC_REFRESH_MS; /* force fresh time */
    RefreshTime();
    (void)DataLogger_LogNow(&s_now, s_time_valid, &data, online);
}

void RESS_App_SetLogInterval(uint32_t minutes)
{
    DataLogger_SetIntervalMin(minutes);
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

    DS3231_Time_t t = { year, month, date, hour, minute, second };
    if (DS3231_SetTime(&s_rtc, &t) != HAL_OK)
    {
        RESS_LOG("RTC", "SETTIME FAILED (I2C error)");
        return false;
    }

    s_last_rtc_tick = HAL_GetTick() - RESS_RTC_REFRESH_MS;
    RefreshTime();
    RESS_LOG("RTC", "time set, valid=%d", (int)s_time_valid);
    return true;
}
