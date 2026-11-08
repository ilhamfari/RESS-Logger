#pragma once

#include <Arduino.h>
#include "nvs_config.h"
#include "stm32_uart.h"

typedef void (*RelayCmdReceivedCallback)(uint8_t channel, uint8_t state);
typedef void (*TelemetryDeliveredCallback)(uint32_t seq, bool success);

class MQTTManager {
public:
    static void init(const SystemConfig &cfg);
    static void process();
    static bool isConnected();
    static bool publishTelemetry(const TelemetrySnapshot &snap, uint32_t epoch_ts);
    static void setOnRelayCmd(RelayCmdReceivedCallback cb);
    static void setOnTelemetryDelivered(TelemetryDeliveredCallback cb);
    static void reloadConfig(const SystemConfig &cfg);

private:
    static void connect();
    static void onMqttMessage(String &topic, String &payload);
};
