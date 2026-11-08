#pragma once

#include <Arduino.h>
#include "nvs_config.h"

typedef void (*WebRelayToggleCallback)(uint8_t channel, uint8_t state);

class WebServerManager {
public:
    static void init(SystemConfig &cfg);
    static void process();
    static void setOnRelayToggle(WebRelayToggleCallback cb);

private:
    static void handleRoot();
    static void handleSettings();
    static void handleSaveSettings();
    static void handleRelayToggle();
    static void handleRestart();
    static void handleNotFound();
    static void handleApiStatus();
    static void handleWifiScan();
    static void handleNtpSync();
};
