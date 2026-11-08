#pragma once

#include <Arduino.h>

// Firmware & Contract Information
#define FIRMWARE_VERSION       "0.1.0"
#define CONTRACT_VERSION       1

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

// Default System Configurations
#define DEFAULT_WIFI_SSID      "RESS-Network"
#define DEFAULT_WIFI_PASS      "ress123456"

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

// Minimum valid epoch timestamp (e.g., 2024-01-01 00:00:00 UTC = 1704067200)
#define MIN_VALID_EPOCH        1704067200ULL
