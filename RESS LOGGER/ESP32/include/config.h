#pragma once

#include <Arduino.h>

// Firmware & Contract Information
#define FIRMWARE_VERSION       "0.1.0"
#define CONTRACT_VERSION       2

// Hardware UART Pins for STM32 Link (From PCB Schematic)
// STM32 PD8 (USART3 TX) connects to ESP32 Pin 11 (GPIO 18)
// STM32 PD9 (USART3 RX) connects to ESP32 Pin 10 (GPIO 17)
#define STM32_UART_NUM         1
#define STM32_RX_PIN           18
#define STM32_TX_PIN           17
#define STM32_BAUD_RATE        115200

// Indicator Pins (From PCB Schematic)
#define LED_PIN                21
#define BUZZER_PIN             15

// 128x64 SSD1306 OLED on the ESP32-S3 I2C bus.  Change these values here if
// the PCB routes the display differently or uses another I2C address.
#define OLED_SDA_PIN           8
#define OLED_SCL_PIN           9
#define OLED_I2C_ADDRESS       0x3C
#define OLED_WIDTH             128
#define OLED_HEIGHT            64

// Default System Configurations
#define DEFAULT_WIFI_SSID      "IoT"
#define DEFAULT_WIFI_PASS      "agusramelan"

#define DEFAULT_AP_SSID        "RESS_GATEWAY_AP"
#define DEFAULT_AP_PASS        "12345678"

#define DEFAULT_MQTT_SERVER    "ressmqtt.sems.web.id"
#define DEFAULT_MQTT_PORT      11833
#define DEFAULT_BOARD_ID       "RB261107RFT001"
#define DEFAULT_MQTT_USER      "RB261107RFT001"
#define DEFAULT_MQTT_PASS      "RB261107RFT001"

#define DEFAULT_INTERVAL_MIN   1

// NTP Settings
#define NTP_SERVER_1           "pool.ntp.org"
#define NTP_SERVER_2           "time.nist.gov"
#define NTP_SERVER_3           "time.google.com"
#define TIMEZONE_OFFSET_S      0  // UTC per contract requirement
#define DAYLIGHT_OFFSET_S      0

// Minimum valid epoch timestamp (Contract: earlier ts is "unset clock", 2026-01-01 00:00:00 UTC = 1767225600)
#define MIN_VALID_EPOCH        1767225600ULL
