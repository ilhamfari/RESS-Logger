#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include "config.h"
#include "nvs_config.h"
#include "ntp_time.h"
#include "stm32_uart.h"
#include "mqtt_client.h"
#include "web_server.h"

static SystemConfig g_cfg;
static uint32_t s_last_poll_tick = 0;
static bool s_first_poll_done = false;
static uint32_t s_last_led_tick = 0;
static bool s_led_state = false;

static void onSnapshotReceived(const TelemetrySnapshot &snap) {
    Serial.printf("[MAIN] Full snapshot received from STM32 (seq=%lu)\r\n", (unsigned long)snap.seq);

    uint32_t epoch = NTPTimeManager::getEpochSeconds();
    if (epoch < MIN_VALID_EPOCH) {
        Serial.printf("[MAIN] Warning: NTP not ready, using snapshot seq as ts (%lu)\r\n", (unsigned long)snap.seq);
        epoch = snap.seq;
    }

    // Publish telemetry to MQTT broker
    MQTTManager::publishTelemetry(snap, epoch);
}

static void onTelemetryDelivered(uint32_t seq, bool success) {
    if (success) {
        Serial.printf("[MAIN] Sending ACK to STM32 for seq=%lu\r\n", (unsigned long)seq);
        STM32UARTBridge::sendAckTelemetry(seq, true);
    } else {
        Serial.printf("[MAIN] Telemetry delivery failed for seq=%lu, no ACK sent\r\n", (unsigned long)seq);
    }
}

static void onRelayCommand(uint8_t channel, uint8_t state) {
    Serial.printf("[MAIN] Relaying command to STM32: Ch %u -> %u\r\n", channel, state);
    NVSConfigManager::saveRelayState(channel, state);
    STM32UARTBridge::sendRelayCommand(channel, state);
}

static void onRelayAck(uint8_t channel, uint8_t state) {
    Serial.printf("[MAIN] STM32 confirmed Relay %u is now %u\r\n", channel, state);
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

    // 1. Load Configurations from Flash
    NVSConfigManager::loadConfig(g_cfg);
    Serial.printf("[CFG] Board ID : %s\r\n", g_cfg.board_id);
    Serial.printf("[CFG] Target AP: %s\r\n", g_cfg.wifi_ssid);
    Serial.printf("[CFG] MQTT Srv : %s:%u\r\n", g_cfg.mqtt_server, g_cfg.mqtt_port);
    Serial.printf("[CFG] Interval : %u min\r\n", g_cfg.send_interval_min);

    // 2. Setup STM32 UART Bridge
    STM32UARTBridge::init();
    STM32UARTBridge::setOnSnapshotReceived(onSnapshotReceived);
    STM32UARTBridge::setOnRelayAck(onRelayAck);
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

    // The first poll is sent as soon as the STM32 has confirmed the NTP time
    // (see loop()), so every SD row on the STM32 carries a valid timestamp.
    s_last_poll_tick = millis();
}

void loop() {
    // Process core event loops
    STM32UARTBridge::process();
    MQTTManager::process();
    WebServerManager::process();
    NTPTimeManager::process();

    // Periodic Telemetry Polling to STM32
    uint32_t now = millis();
    uint32_t interval_ms = (uint32_t)g_cfg.send_interval_min * 60000UL;
    if (interval_ms == 0) interval_ms = 60000UL;

    // Poll only once the STM32 clock is set (STM32 logs one SD row per poll)
    if (STM32UARTBridge::isTimeAcked()) {
        if (!s_first_poll_done || (now - s_last_poll_tick >= interval_ms)) {
            s_first_poll_done = true;
            s_last_poll_tick = now;
            Serial.println("[MAIN] Polling STM32 for telemetry ($REQ,TELE)...");
            STM32UARTBridge::requestTelemetry();
        }
    } else if ((now - s_last_poll_tick) >= 30000UL) {
        s_last_poll_tick = now;
        Serial.println("[MAIN] Waiting for NTP time to be confirmed by STM32 before polling...");
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
