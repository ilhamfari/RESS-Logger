#include "display_status.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>

#include "config.h"
#include "mqtt_client.h"
#include "ntp_time.h"
#include "stm32_uart.h"

static Adafruit_SSD1306 s_display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool s_ready = false;
static uint32_t s_last_draw = 0;
static uint32_t s_last_scroll = 0;
static uint8_t s_first_line = 0;

void DisplayStatus::init() {
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    Wire.beginTransmission(OLED_I2C_ADDRESS);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[OLED] No I2C response at 0x%02X (SDA=%d SCL=%d)\r\n",
                      OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
        return;
    }
    s_ready = s_display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);
    if (!s_ready) {
        Serial.printf("[OLED] SSD1306 not found at 0x%02X (SDA=%d SCL=%d)\r\n",
                      OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
        return;
    }

    s_display.clearDisplay();
    s_display.setTextColor(SSD1306_WHITE);
    s_display.setTextSize(1);
    s_display.setCursor(18, 26);
    s_display.print("RESS LOGGER");
    s_display.display();
    Serial.printf("[OLED] SSD1306 ready at 0x%02X (SDA=%d SCL=%d)\r\n",
                  OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
}

void DisplayStatus::process() {
    if (!s_ready) return;

    uint32_t now_ms = millis();
    if (now_ms - s_last_scroll >= 1800U) {
        s_last_scroll = now_ms;
        s_first_line = (uint8_t)((s_first_line + 1U) % 8U);
    }
    if (now_ms - s_last_draw < 250U) return;
    s_last_draw = now_ms;

    char lines[8][24];
    bool wifi_ok = WiFi.status() == WL_CONNECTED;
    IPAddress ip = wifi_ok ? WiFi.localIP() : WiFi.softAPIP();
    snprintf(lines[0], sizeof(lines[0]), "IP %s", ip.toString().c_str());
    if (wifi_ok) snprintf(lines[1], sizeof(lines[1]), "WiFi %d dBm", WiFi.RSSI());
    else snprintf(lines[1], sizeof(lines[1]), "WiFi DISCONNECTED");
    snprintf(lines[2], sizeof(lines[2]), "MQTT %s", MQTTManager::isConnected() ? "CONNECTED" : "OFFLINE");

    uint32_t epoch = NTPTimeManager::getEpochSeconds();
    if (epoch >= MIN_VALID_EPOCH) {
        time_t raw = (time_t)epoch;
        struct tm utc;
        gmtime_r(&raw, &utc);
        snprintf(lines[3], sizeof(lines[3]), "Clock %02d:%02d:%02d", utc.tm_hour, utc.tm_min, utc.tm_sec);
    } else {
        snprintf(lines[3], sizeof(lines[3]), "Clock --:--:--");
    }

    const TelemetrySnapshot &snap = STM32UARTBridge::getLatestSnapshot();
    snprintf(lines[4], sizeof(lines[4]), "MODBUS DEVICES");
    snprintf(lines[5], sizeof(lines[5]), "PM= %s", snap.complete ? (snap.larkin.online ? "ONLINE" : "OFFLINE") : "WAIT");
    snprintf(lines[6], sizeof(lines[6]), "GW= %s", snap.complete ? (snap.goodwe.online ? "ONLINE" : "OFFLINE") : "WAIT");
    snprintf(lines[7], sizeof(lines[7]), "SG= %s", snap.complete ? (snap.sungrow.online ? "ONLINE" : "OFFLINE") : "WAIT");

    s_display.clearDisplay();
    s_display.setCursor(0, 0);
    // Six rows are shown; the rotating start line provides a gentle vertical
    // status scroll while the clock continues updating every second.
    for (uint8_t row = 0; row < 6U; row++) {
        uint8_t index = (uint8_t)((s_first_line + row) % 8U);
        s_display.setCursor(0, row * 10U);
        s_display.print(lines[index]);
    }
    s_display.display();
}
