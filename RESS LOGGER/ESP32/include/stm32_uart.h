#pragma once

#include <Arduino.h>

struct LarkinTelemetry {
    bool online;
    float ua, ub, uc;
    float ia, ib, ic;
    float p_total, q_total, s_total;
    float pf_avg, freq;
    bool has_e_imp;
    float e_imp;
    bool has_e_exp;
    float e_exp;
};

struct InverterTelemetry {
    bool online;
    bool has_vpv;
    float vpv;
    bool has_ipv;
    float ipv;
    bool has_ppv;
    float ppv;
    bool has_energy;
    float energy_kwh;
    bool has_pout;
    float pout;
    bool has_pload;
    float pload;
    bool has_temp;
    float temp;
    bool has_status;
    char status[32];
};

struct RelayState {
    uint8_t r1, r2, r3, r4;
    bool valid;
};

struct TelemetrySnapshot {
    uint32_t seq;
    LarkinTelemetry larkin;
    InverterTelemetry goodwe;
    InverterTelemetry sungrow;
    RelayState relay;
    bool complete;
    uint32_t received_tick;
};

typedef void (*SnapshotCallback)(const TelemetrySnapshot &snapshot);
typedef void (*RelayAckCallback)(uint8_t channel, uint8_t state);

class STM32UARTBridge {
public:
    static void init();
    static void process();

    static bool requestTelemetry(uint32_t scheduled_epoch);
    static bool sendAckTelemetry(uint32_t seq, bool success);
    static bool sendRelayCommand(uint8_t channel, uint8_t state);
    static bool sendGoodWeEnable(bool enabled);
    static bool sendTimeSync(uint16_t year, uint8_t month, uint8_t date,
                             uint8_t hour, uint8_t minute, uint8_t second, uint32_t epoch);

    static uint8_t calcChecksum(const char *payload);

    static void setOnSnapshotReceived(SnapshotCallback cb);
    static void setOnRelayAck(RelayAckCallback cb);

    // True once the STM32 acknowledged our latest $CMD,TIME. Cleared when the
    // STM32 asks for the time again ($REQ,TIME), e.g. after it was reset.
    static bool isTimeAcked();
    static void setOnTimeRequest(void (*cb)());

    static const TelemetrySnapshot& getLatestSnapshot();
    static const RelayState& getLatestRelay();

private:
    static void handleLine(char *line);
    static void parseLarkin(char *args[], size_t count);
    static void parseInverter(InverterTelemetry &inv, char *args[], size_t count);
    static void parseRelay(char *args[], size_t count);
};
