#pragma once

#include <Arduino.h>
#include "nvs_config.h"

class GatewayWiFi {
public:
    static void init(const SystemConfig &cfg);
    static void process();

    // Starts an asynchronous scan. If a station connection is in progress it
    // is paused first so the single WiFi radio can keep serving the AP.
    static bool requestScan();
    static int16_t scanComplete();
    static void deleteScanResults();

    static bool isConnecting();
    static bool isAccessPointStarted();

private:
    GatewayWiFi() = delete;
};
