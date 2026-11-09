#include "gateway_wifi.h"

#include <WiFi.h>

#include "config.h"

namespace {

enum class StationState : uint8_t {
    IDLE,
    CONNECTING,
    CONNECTED,
    RETRY_WAIT,
};

constexpr uint32_t CONNECT_TIMEOUT_MS = 15000UL;
constexpr uint32_t INITIAL_RETRY_MS = 15000UL;
constexpr uint32_t MAX_RETRY_MS = 300000UL;
constexpr uint32_t LOST_CONNECTION_RETRY_MS = 5000UL;
constexpr uint32_t AP_RETRY_MS = 5000UL;

const SystemConfig *s_cfg = nullptr;
StationState s_state = StationState::IDLE;
uint32_t s_attempt_started_ms = 0;
uint32_t s_next_retry_ms = 0;
uint32_t s_retry_delay_ms = INITIAL_RETRY_MS;
uint32_t s_last_ap_attempt_ms = 0;
bool s_ap_started = false;

bool deadlineReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

bool startAccessPoint() {
    s_last_ap_attempt_ms = millis();
    s_ap_started = WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS, 1, false, 4);

    Serial.printf("[AP] Start=%s, SSID=%s, IP=%s, requested_channel=1\r\n",
                  s_ap_started ? "OK" : "FAILED",
                  DEFAULT_AP_SSID,
                  WiFi.softAPIP().toString().c_str());
    return s_ap_started;
}

void startStationAttempt() {
    if (!s_cfg || s_cfg->wifi_ssid[0] == '\0') {
        Serial.println("[WIFI] No station SSID configured; AP remains active");
        s_state = StationState::IDLE;
        return;
    }

    Serial.printf("[WIFI] Connecting to '%s' (AP remains active)...\r\n",
                  s_cfg->wifi_ssid);
    WiFi.begin(s_cfg->wifi_ssid, s_cfg->wifi_pass);
    s_attempt_started_ms = millis();
    s_state = StationState::CONNECTING;
}

void scheduleRetry(uint32_t delay_ms) {
    s_next_retry_ms = millis() + delay_ms;
    s_state = StationState::RETRY_WAIT;
}

} // namespace

void GatewayWiFi::init(const SystemConfig &cfg) {
    s_cfg = &cfg;

    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_AP_STA);
    delay(100);

    startAccessPoint();
    startStationAttempt();
}

void GatewayWiFi::process() {
    const uint32_t now = millis();

    if (!s_ap_started && now - s_last_ap_attempt_ms >= AP_RETRY_MS) {
        Serial.println("[AP] Retrying access point startup...");
        startAccessPoint();
    }

    // An active scan owns the single radio briefly. The AP remains configured,
    // and station retries resume after the scan completes.
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        return;
    }

    switch (s_state) {
    case StationState::CONNECTING:
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WIFI] Connected to '%s', IP=%s, channel=%d, RSSI=%d dBm\r\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                          WiFi.channel(), WiFi.RSSI());
            s_retry_delay_ms = INITIAL_RETRY_MS;
            s_state = StationState::CONNECTED;
        } else if (now - s_attempt_started_ms >= CONNECT_TIMEOUT_MS) {
            WiFi.disconnect(false, false);
            Serial.printf("[WIFI] Connection failed; retrying in %lu s (AP stays active)\r\n",
                          static_cast<unsigned long>(s_retry_delay_ms / 1000UL));
            scheduleRetry(s_retry_delay_ms);
            const uint32_t doubled_delay = s_retry_delay_ms * 2U;
            s_retry_delay_ms = (doubled_delay < MAX_RETRY_MS)
                                   ? doubled_delay
                                   : MAX_RETRY_MS;
        }
        break;

    case StationState::CONNECTED:
        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[WIFI] Connection lost; retrying in %lu s\r\n",
                          static_cast<unsigned long>(LOST_CONNECTION_RETRY_MS / 1000UL));
            scheduleRetry(LOST_CONNECTION_RETRY_MS);
        }
        break;

    case StationState::RETRY_WAIT:
        if (deadlineReached(now, s_next_retry_ms)) {
            startStationAttempt();
        }
        break;

    case StationState::IDLE:
        break;
    }
}

bool GatewayWiFi::requestScan() {
    int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) {
        return true;
    }

    if (s_state == StationState::CONNECTING) {
        WiFi.disconnect(false, false);
        scheduleRetry(LOST_CONNECTION_RETRY_MS);
        Serial.println("[WIFI] Station attempt paused for requested AP scan");
    }

    WiFi.scanDelete();
    result = WiFi.scanNetworks(true, true);
    if (result == WIFI_SCAN_RUNNING || result >= 0) {
        Serial.println("[WIFI] Asynchronous network scan started");
        return true;
    }

    Serial.printf("[WIFI] Unable to start network scan (code=%d)\r\n", result);
    return false;
}

int16_t GatewayWiFi::scanComplete() {
    return WiFi.scanComplete();
}

void GatewayWiFi::deleteScanResults() {
    WiFi.scanDelete();
}

bool GatewayWiFi::isConnecting() {
    return s_state == StationState::CONNECTING;
}

bool GatewayWiFi::isAccessPointStarted() {
    return s_ap_started;
}
