#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include "config.h"
#include "nvs_config.h"
#include "ntp_time.h"
#include "stm32_uart.h"
#include "mqtt_client.h"
#include "web_server.h"
#include "display_status.h"

static SystemConfig g_cfg;
static uint32_t s_next_poll_epoch = 0;
static bool s_poll_pending = false;
static uint32_t s_poll_sent_tick = 0;
static bool s_replay_active = false;
static bool s_replay_request_due = false;
static bool s_publishing_replay = false;
static bool s_last_mqtt_connected = false;
static uint32_t s_last_led_tick = 0;
static bool s_led_state = false;

static void onSnapshotReceived(const TelemetrySnapshot &snap) {
    Serial.printf("[MAIN] Full snapshot received from STM32 (seq=%lu)\r\n", (unsigned long)snap.seq);

    /* Use the scheduler's minute bucket, not the time at which UART parsing
     * happened.  Queue latency therefore never changes :00 to :27, etc. */
    uint32_t epoch = snap.epoch;
    if (epoch < MIN_VALID_EPOCH) {
        // Never invent a timestamp: the broker contract rejects ts < 2026-01-01.
        // No ACK is sent, so the STM32 keeps this SD row as SENT=0.
        Serial.println("[MAIN] Clock not valid yet, telemetry not published");
        return;
    }

    s_poll_pending = false;

    s_publishing_replay = snap.replayed;
    bool published = MQTTManager::publishTelemetry(snap, epoch);
    if (!published && snap.replayed) {
        s_replay_request_due = true;
        s_poll_sent_tick = millis();
    }
}

static void onTelemetryDelivered(uint32_t seq, bool success) {
    if (success) {
        Serial.printf("[MAIN] Sending ACK to STM32 for seq=%lu\r\n", (unsigned long)seq);
        STM32UARTBridge::sendAckTelemetry(seq, true);
        if (s_publishing_replay) s_replay_request_due = true;
    } else {
        Serial.printf("[MAIN] Telemetry delivery failed for seq=%lu, no ACK sent\r\n", (unsigned long)seq);
    }
}

static void onPendingEmpty() {
    s_poll_pending = false;
    s_replay_active = false;
    s_replay_request_due = false;
    Serial.println("[MAIN] Durable STM32 telemetry queue is empty");
}

static void onRelayCommand(uint8_t channel, uint8_t state) {
    Serial.printf("[MAIN] Relaying command to STM32: Ch %u -> %u\r\n", channel, state);
    NVSConfigManager::saveRelayState(channel, state);
    STM32UARTBridge::sendRelayCommand(channel, state);
}

static void onRelayAck(uint8_t channel, uint8_t state) {
    Serial.printf("[MAIN] STM32 confirmed Relay %u is now %u\r\n", channel, state);
}

static void onGoodWeToggle(bool enabled) {
    g_cfg.goodwe_enabled = enabled;
    NVSConfigManager::saveConfig(g_cfg);
    STM32UARTBridge::sendGoodWeEnable(enabled);

    if (!enabled) {
        s_poll_pending = false;
        s_next_poll_epoch = 0U;
    }
    Serial.printf("[MAIN] GoodWe acquisition %s from web UI\r\n", enabled ? "ENABLED" : "DISABLED");
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\r\n========================================");
    Serial.printf("RESS Logger ESP32-S3 Gateway v%s\r\n", FIRMWARE_VERSION);
    Serial.println("========================================");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    DisplayStatus::init();

    // 1. Load Configurations from Flash
    NVSConfigManager::loadConfig(g_cfg);
    Serial.printf("[CFG] Board ID : %s\r\n", g_cfg.board_id);
    Serial.printf("[CFG] Target AP: %s\r\n", g_cfg.wifi_ssid);
    Serial.printf("[CFG] MQTT Srv : %s:%u\r\n", g_cfg.mqtt_server, g_cfg.mqtt_port);
    Serial.printf("[CFG] Interval : %u min\r\n", g_cfg.send_interval_min);
    Serial.printf("[CFG] GoodWe   : %s\r\n", g_cfg.goodwe_enabled ? "ENABLED" : "DISABLED");

    // 2. Setup STM32 UART Bridge
    STM32UARTBridge::init();
    STM32UARTBridge::setOnSnapshotReceived(onSnapshotReceived);
    STM32UARTBridge::setOnRelayAck(onRelayAck);
    STM32UARTBridge::setOnPendingEmpty(onPendingEmpty);
    STM32UARTBridge::setOnTimeRequest([]() { NTPTimeManager::syncToSTM32(); });

    // Restore last saved relay states to STM32 from NVS
    uint8_t r1 = 0, r2 = 0, r3 = 0, r4 = 0;
    NVSConfigManager::loadRelayStates(r1, r2, r3, r4);
    Serial.printf("[MAIN] Restoring relay states from NVS: R1=%u, R2=%u, R3=%u, R4=%u\r\n", r1, r2, r3, r4);
    delay(50);
    STM32UARTBridge::sendRelayCommand(1, r1);
    delay(30);
    STM32UARTBridge::sendRelayCommand(2, r2);
    delay(30);
    STM32UARTBridge::sendRelayCommand(3, r3);
    delay(30);
    STM32UARTBridge::sendRelayCommand(4, r4);
    delay(30);
    STM32UARTBridge::sendGoodWeEnable(g_cfg.goodwe_enabled);

    // 3. Connect to WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_cfg.wifi_ssid, g_cfg.wifi_pass);
    Serial.printf("[WIFI] Connecting to '%s'...", g_cfg.wifi_ssid);

    uint32_t start_attempt = millis();
    bool wifi_connected = false;
    while (millis() - start_attempt < 12000) {
        if (WiFi.status() == WL_CONNECTED) {
            wifi_connected = true;
            break;
        }
        delay(500);
        Serial.print(".");
    }

    if (wifi_connected) {
        Serial.println("\r\n[WIFI] Connected! IP: " + WiFi.localIP().toString());
        WiFi.setSleep(false);          // modem sleep causes flaky long-lived TCP/MQTT sessions
        WiFi.setAutoReconnect(true);
        digitalWrite(LED_PIN, HIGH);
    } else {
        Serial.println("\r\n[WIFI] Failed to connect! Starting fallback Access Point...");
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(DEFAULT_AP_SSID, DEFAULT_AP_PASS);
        Serial.printf("[AP] SSID: %s, IP: %s\r\n", DEFAULT_AP_SSID, WiFi.softAPIP().toString().c_str());
    }

    // 4. Start mDNS
    if (MDNS.begin("ress-gateway")) {
        Serial.println("[MDNS] Responder started: http://ress-gateway.local");
    }

    // 5. Setup NTP & Initial Time Sync
    NTPTimeManager::init();
    if (wifi_connected) {
        Serial.print("[NTP] Waiting for NTP sync...");
        uint32_t t0 = millis();
        while (millis() - t0 < 3000) {
            if (NTPTimeManager::isTimeSynchronized()) {
                Serial.println(" OK!");
                NTPTimeManager::syncToSTM32();
                break;
            }
            delay(200);
            Serial.print(".");
        }
        if (!NTPTimeManager::isTimeSynchronized()) {
            Serial.println(" Pending (will auto-sync in background).");
        }
    }

    // 6. Setup MQTT
    MQTTManager::init(g_cfg);
    MQTTManager::setOnRelayCmd(onRelayCommand);
    MQTTManager::setOnTelemetryDelivered(onTelemetryDelivered);

    // 7. Setup Web Server
    WebServerManager::init(g_cfg);
    WebServerManager::setOnRelayToggle(onRelayCommand);
    WebServerManager::setOnGoodWeToggle(onGoodWeToggle);

    // The first poll is scheduled on the next real interval boundary.
    // For example, a 5-minute interval uses :00, :05, :10 ... exactly.
    s_next_poll_epoch = 0;
}

void loop() {
    // Process core event loops
    STM32UARTBridge::process();
    MQTTManager::process();
    WebServerManager::process();
    NTPTimeManager::process();
    DisplayStatus::process();

    uint32_t now = millis();

    bool mqtt_connected = MQTTManager::isConnected();
    if (mqtt_connected && !s_last_mqtt_connected) {
        s_replay_active = true;
        s_replay_request_due = true;
        Serial.println("[MAIN] MQTT connected; starting oldest-first SD replay");
    }
    s_last_mqtt_connected = mqtt_connected;

    // Auto-reconnect WiFi if disconnected
    static uint32_t s_last_wifi_retry = 0;
    if (WiFi.status() != WL_CONNECTED && (now - s_last_wifi_retry >= 10000)) {
        s_last_wifi_retry = now;
        Serial.printf("[WIFI] Reconnecting to '%s'...\r\n", g_cfg.wifi_ssid);
        WiFi.reconnect();
    }

    // Wall-clock scheduler: execute from the cooperative task queue, while
    // preserving the intended HH:MM:00 timestamp if servicing is delayed.
    uint32_t interval_s = (uint32_t)g_cfg.send_interval_min * 60UL;
    if (interval_s == 0U) interval_s = 60U;
    uint32_t epoch_now = NTPTimeManager::getEpochSeconds();

    if (mqtt_connected && s_replay_active) {
        if (s_poll_pending && now - s_poll_sent_tick >= 30000UL) {
            s_poll_pending = false;
            s_replay_request_due = true;
            s_poll_sent_tick = now;
            Serial.println("[MAIN] Replay response timeout; will retry");
        }
        if (s_replay_request_due && !s_poll_pending && now - s_poll_sent_tick >= 1000UL) {
            s_replay_request_due = false;
            s_poll_pending = true;
            s_poll_sent_tick = now;
            if (!STM32UARTBridge::requestPending()) {
                s_poll_pending = false;
                s_replay_request_due = true;
            }
        }
    } else if (g_cfg.goodwe_enabled && STM32UARTBridge::isTimeAcked() && epoch_now >= MIN_VALID_EPOCH) {
        if (s_next_poll_epoch == 0U) {
            s_next_poll_epoch = ((epoch_now / interval_s) + 1U) * interval_s;
            Serial.printf("[MAIN] Next telemetry minute epoch=%lu\r\n",
                          (unsigned long)s_next_poll_epoch);
        }

        // Recover from a missing UART response without blocking later slots.
        if (s_poll_pending && (now - s_poll_sent_tick >= 30000UL)) {
            Serial.println("[MAIN] Telemetry response timeout; releasing queue slot");
            s_poll_pending = false;
        }

        if (!s_poll_pending && epoch_now >= s_next_poll_epoch) {
            uint32_t scheduled_epoch = s_next_poll_epoch;
            // Do not burst old samples after a long outage.  Keep the newest
            // elapsed interval and advance the next task from wall-clock time.
            if ((epoch_now - scheduled_epoch) >= interval_s) {
                scheduled_epoch = (epoch_now / interval_s) * interval_s;
            }
            s_next_poll_epoch = scheduled_epoch + interval_s;
            s_poll_pending = true;
            s_poll_sent_tick = now;
            Serial.printf("[MAIN] Queue telemetry for epoch=%lu (:00)\r\n",
                          (unsigned long)scheduled_epoch);
            if (!STM32UARTBridge::requestTelemetry(scheduled_epoch, (int16_t)WiFi.RSSI(),
                                                    millis() / 1000U, FIRMWARE_VERSION)) {
                s_poll_pending = false;
            }
        }
    } else {
        s_next_poll_epoch = 0U;
        s_poll_pending = false;
        static uint32_t s_last_time_wait_log = 0;
        if (g_cfg.goodwe_enabled && (now - s_last_time_wait_log) >= 30000UL) {
            s_last_time_wait_log = now;
            Serial.println("[MAIN] Waiting for NTP time to be confirmed by STM32 before polling...");
        }
    }

    // Status LED blink
    if (now - s_last_led_tick >= 1000) {
        s_last_led_tick = now;
        s_led_state = !s_led_state;
        if (WiFi.status() == WL_CONNECTED) {
            // Steady ON with short blink
            digitalWrite(LED_PIN, s_led_state ? HIGH : LOW);
        } else {
            // Fast toggle in AP mode
            digitalWrite(LED_PIN, s_led_state);
        }
    }
}
