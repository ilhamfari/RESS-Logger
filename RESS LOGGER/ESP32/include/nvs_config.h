#pragma once

#include <Arduino.h>

struct SystemConfig {
    char wifi_ssid[64];
    char wifi_pass[64];

    char mqtt_server[128];
    uint16_t mqtt_port;
    char board_id[32];
    char mqtt_user[64];
    char mqtt_pass[64];

    uint16_t send_interval_min;

    char topic_telemetry[128];
    char topic_status[128];
    char topic_cmd[128];
};

class NVSConfigManager {
public:
    static void init();
    static bool loadConfig(SystemConfig &cfg);
    static bool saveConfig(const SystemConfig &cfg);
    static void resetDefaults(SystemConfig &cfg);
    static void updateDerivedTopics(SystemConfig &cfg);

    static void saveRelayState(uint8_t ch, uint8_t state);
    static void loadRelayStates(uint8_t &r1, uint8_t &r2, uint8_t &r3, uint8_t &r4);
    static uint8_t getRelayState(uint8_t ch);
};
