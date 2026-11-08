#include "ntp_time.h"
#include "config.h"
#include "stm32_uart.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <sys/time.h>
#include "esp_sntp.h"

// ---------------------------------------------------------------------------
// Time sources, in order of preference:
//   1. SNTP over UDP/123 (rotating through several servers)
//   2. HTTP "Date" header of a well-known plain-HTTP endpoint (works when a
//      firewall blocks UDP 123 but allows web traffic)
//   3. Browser clock from the web UI (works with no internet at all)
// The first valid time is pushed to the STM32 until it acknowledges.
// ---------------------------------------------------------------------------

static const char *const NTP_POOL[] = {
    "pool.ntp.org", "time.google.com", "time.cloudflare.com",
    "time.nist.gov", "time.windows.com", "ntp.ubuntu.com",
};
static const size_t NTP_POOL_COUNT = sizeof(NTP_POOL) / sizeof(NTP_POOL[0]);

static const char *const HTTP_TIME_URLS[] = {
    "http://clients3.google.com/generate_204",
    "http://www.msftconnecttest.com/connecttest.txt",
    "http://neverssl.com/",
};
static const size_t HTTP_TIME_URL_COUNT = sizeof(HTTP_TIME_URLS) / sizeof(HTTP_TIME_URLS[0]);

static const uint32_t SNTP_RETRY_MS = 15000;    // rotate server set while unsynced
static const uint32_t HTTP_RETRY_MS = 20000;    // HTTP fallback attempt period
static const uint32_t DIAG_PERIOD_MS = 10000;   // diagnostics print while unsynced
static const uint32_t STM32_ACKED_REFRESH_MS = 600000UL;
static const uint32_t STM32_UNACKED_RETRY_MS = 5000;

static uint32_t s_last_ntp_check = 0;
static uint32_t s_last_stm32_sync = 0;
static uint32_t s_last_sntp_retry = 0;
static uint32_t s_last_http_try = 0;
static uint32_t s_last_diag = 0;
static uint32_t s_unsynced_since = 0;
static size_t   s_pool_index = 0;
static bool s_has_synced_stm32 = false;
static bool s_last_wifi_connected = false;
static volatile bool s_sntp_done = false;       // set from the SNTP task callback
static bool s_sntp_reported = false;
static const char *s_source = "none";

static void sntpSyncCallback(struct timeval *tv) {
    (void)tv;
    s_sntp_done = true;
}

static void startSntp() {
    // Three servers starting at the current rotation index.
    const char *a = NTP_POOL[s_pool_index % NTP_POOL_COUNT];
    const char *b = NTP_POOL[(s_pool_index + 1) % NTP_POOL_COUNT];
    const char *c = NTP_POOL[(s_pool_index + 2) % NTP_POOL_COUNT];
    configTime(TIMEZONE_OFFSET_S, DAYLIGHT_OFFSET_S, a, b, c);
    Serial.printf("[NTP] SNTP servers: %s, %s, %s\r\n", a, b, c);
}

void NTPTimeManager::init() {
    sntp_set_time_sync_notification_cb(sntpSyncCallback);
    startSntp();
    s_unsynced_since = millis();
    s_last_sntp_retry = millis();
    Serial.println("[NTP] Service initialized (SNTP + HTTP fallback)");
}

void NTPTimeManager::forceSync() {
    s_pool_index = (s_pool_index + 3) % NTP_POOL_COUNT;
    startSntp();
}

bool NTPTimeManager::isTimeSynchronized() {
    time_t now;
    time(&now);
    return ((uint64_t)now >= MIN_VALID_EPOCH);
}

uint32_t NTPTimeManager::getEpochSeconds() {
    time_t now;
    time(&now);
    return (uint32_t)now;
}

const char *NTPTimeManager::getSource() {
    return s_source;
}

String NTPTimeManager::getFormattedTime() {
    time_t now;
    time(&now);
    if ((uint64_t)now < MIN_VALID_EPOCH) {
        return "Not Synchronized";
    }
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
    return String(buf);
}

bool NTPTimeManager::setFromEpoch(uint32_t epoch, const char *source) {
    if ((uint64_t)epoch < MIN_VALID_EPOCH) {
        return false;
    }
    struct timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        return false;
    }
    s_source = source;
    s_has_synced_stm32 = false;   // push the new time to the STM32 right away
    Serial.printf("[NTP] System clock set from %s: %s\r\n", source, getFormattedTime().c_str());
    return true;
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

// Parses "Sat, 08 Nov 2026 10:11:12 GMT".
static bool parseHttpDate(const String &s, uint32_t &epoch) {
    static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char mon[4] = {0};
    int day, year, hh, mm, ss;
    if (sscanf(s.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) {
        return false;
    }
    int m = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, MON[i], 3) == 0) { m = i + 1; break; }
    }
    if (m == 0) return false;
    int64_t days = daysFromCivil(year, (unsigned)m, (unsigned)day);
    int64_t t = days * 86400LL + hh * 3600LL + mm * 60LL + ss;
    if (t < 0 || t > 0xFFFFFFFFLL) return false;
    epoch = (uint32_t)t;
    return true;
}

static bool httpTimeFallback() {
    for (size_t i = 0; i < HTTP_TIME_URL_COUNT; i++) {
        HTTPClient http;
        http.setConnectTimeout(4000);
        http.setTimeout(4000);
        const char *keys[] = {"Date"};
        http.collectHeaders(keys, 1);
        if (!http.begin(HTTP_TIME_URLS[i])) {
            continue;
        }
        int code = http.GET();
        String date = http.hasHeader("Date") ? http.header("Date") : String();
        http.end();

        Serial.printf("[NTP] HTTP time via %s -> code=%d date='%s'\r\n", HTTP_TIME_URLS[i], code, date.c_str());
        uint32_t epoch;
        if (date.length() > 0 && parseHttpDate(date, epoch)) {
            if (NTPTimeManager::setFromEpoch(epoch, "http-date")) {
                return true;
            }
        }
    }
    return false;
}

static void printDiagnostics() {
    IPAddress ip;
    bool dns_ok = WiFi.hostByName(NTP_POOL[0], ip);
    Serial.printf("[NTP] NOT SYNCED for %lu s | wifi=%d ip=%s gw=%s dns=%s | resolve %s: %s%s\r\n",
                  (unsigned long)((millis() - s_unsynced_since) / 1000),
                  (int)WiFi.status(), WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(),
                  NTP_POOL[0], dns_ok ? "OK " : "FAILED",
                  dns_ok ? ip.toString().c_str() : " (no DNS/internet: check router uplink)");
}

bool NTPTimeManager::syncToSTM32() {
    if (!isTimeSynchronized()) {
        return false;
    }

    time_t now;
    time(&now);
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);

    uint16_t year = (uint16_t)(timeinfo.tm_year + 1900);
    uint8_t month = (uint8_t)(timeinfo.tm_mon + 1);
    uint8_t date  = (uint8_t)timeinfo.tm_mday;
    uint8_t hour  = (uint8_t)timeinfo.tm_hour;
    uint8_t min   = (uint8_t)timeinfo.tm_min;
    uint8_t sec   = (uint8_t)timeinfo.tm_sec;
    uint32_t epoch = (uint32_t)now;

    Serial.printf("[NTP] Sending time to STM32: %04u-%02u-%02u %02u:%02u:%02u UTC (epoch=%lu, source=%s)\r\n",
                  year, month, date, hour, min, sec, (unsigned long)epoch, s_source);

    return STM32UARTBridge::sendTimeSync(year, month, date, hour, min, sec, epoch);
}

void NTPTimeManager::process() {
    uint32_t now_ms = millis();
    bool wifi_connected = (WiFi.status() == WL_CONNECTED);

    if (s_sntp_done) {
        s_sntp_done = false;
        s_source = "sntp";
        if (!s_sntp_reported) {
            s_sntp_reported = true;
            Serial.printf("[NTP] SNTP sync OK: %s\r\n", getFormattedTime().c_str());
        }
        s_has_synced_stm32 = false;   // fresh network time: push to STM32
    }

    // Restart SNTP whenever WiFi (re)connects.
    if (wifi_connected && !s_last_wifi_connected) {
        Serial.println("[NTP] WiFi connected/restored. Restarting SNTP...");
        startSntp();
        s_last_sntp_retry = now_ms;
        s_unsynced_since = now_ms;
    }
    s_last_wifi_connected = wifi_connected;

    if (!isTimeSynchronized()) {
        if (!wifi_connected) {
            return;   // nothing to try without a network
        }

        // 1) keep SNTP alive: rotate to a different server set periodically
        if (now_ms - s_last_sntp_retry >= SNTP_RETRY_MS) {
            s_last_sntp_retry = now_ms;
            s_pool_index = (s_pool_index + 3) % NTP_POOL_COUNT;
            startSntp();
        }

        // 2) HTTP Date fallback if SNTP is still failing
        if (now_ms - s_unsynced_since >= HTTP_RETRY_MS && now_ms - s_last_http_try >= HTTP_RETRY_MS) {
            s_last_http_try = now_ms;
            Serial.println("[NTP] SNTP not answering, trying HTTP Date fallback...");
            httpTimeFallback();
        }

        // 3) tell the user why
        if (now_ms - s_last_diag >= DIAG_PERIOD_MS) {
            s_last_diag = now_ms;
            printDiagnostics();
        }
        return;
    }

    // Time is valid. The STM32 has no RTC: it takes its time only from us.
    //  - until the STM32 acknowledges: resend every 5 s
    //  - afterwards: refresh every 10 minutes
    if (now_ms - s_last_ntp_check < 1000) {
        return;
    }
    s_last_ntp_check = now_ms;

    uint32_t period = STM32UARTBridge::isTimeAcked() ? STM32_ACKED_REFRESH_MS : STM32_UNACKED_RETRY_MS;
    if (!s_has_synced_stm32 || (now_ms - s_last_stm32_sync) >= period) {
        s_has_synced_stm32 = true;
        s_last_stm32_sync = now_ms;
        syncToSTM32();
    }
}
