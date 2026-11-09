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

struct GoodWeTelemetry {
    bool online;
    bool has_device_serial; char device_serial[20];
    bool has_device_type; char device_type[20];
    bool has_error_code; uint32_t error_code;
    bool has_operating_hours; uint32_t operating_hours_total;
    bool has_working_status; char working_status[20];
    bool has_grid_connection_status; char grid_connection_status[20];
    bool has_inverter_temperature; float inverter_temperature;
    bool has_mppt1_voltage; float mppt1_voltage;
    bool has_mppt1_current; float mppt1_current;
    bool has_mppt1_power; float mppt1_power;
    bool has_mppt2_voltage; float mppt2_voltage;
    bool has_mppt2_current; float mppt2_current;
    bool has_mppt2_power; float mppt2_power;
    bool has_pv_power_total; float pv_power_total;
    bool has_insulation_resistance; float insulation_resistance;
    bool has_leakage_current; float leakage_current;
    bool has_active_power; float active_power;
    bool has_reactive_power; float reactive_power;
    bool has_power_factor; float power_factor;
    bool has_phase_a_voltage; float phase_a_voltage;
    bool has_phase_b_voltage; float phase_b_voltage;
    bool has_phase_c_voltage; float phase_c_voltage;
    bool has_phase_a_current; float phase_a_current;
    bool has_phase_b_current; float phase_b_current;
    bool has_phase_c_current; float phase_c_current;
    bool has_phase_a_frequency; float phase_a_frequency;
    bool has_phase_b_frequency; float phase_b_frequency;
    bool has_phase_c_frequency; float phase_c_frequency;
    bool has_grid_frequency; float grid_frequency;
    bool has_line_ab_voltage; float line_ab_voltage;
    bool has_line_bc_voltage; float line_bc_voltage;
    bool has_line_ca_voltage; float line_ca_voltage;
    bool has_energy_total; float energy_total;
    bool has_energy_today; float energy_today;
};

struct GatewayTelemetry {
    bool valid;
    int16_t rssi;
    uint32_t uptime;
    char fw_version[20];
};

struct RelayState {
    uint8_t r1, r2, r3, r4;
    bool valid;
};

struct TelemetrySnapshot {
    uint32_t seq;
    uint32_t epoch;
    bool replayed;
    LarkinTelemetry larkin;
    GoodWeTelemetry goodwe;
    InverterTelemetry sungrow;
    GatewayTelemetry gateway;
    RelayState relay;
    bool complete;
    uint32_t received_tick;
};

typedef void (*SnapshotCallback)(const TelemetrySnapshot &snapshot);
typedef void (*RelayAckCallback)(uint8_t channel, uint8_t state);
typedef void (*PendingEmptyCallback)();

class STM32UARTBridge {
public:
    static void init();
    static void process();

    static bool requestTelemetry(uint32_t scheduled_epoch, int16_t rssi,
                                 uint32_t uptime, const char *fw_version);
    static bool requestPending();
    static bool sendAckTelemetry(uint32_t seq, bool success);
    static bool sendRelayCommand(uint8_t channel, uint8_t state);
    static bool sendGoodWeEnable(bool enabled);
    static bool sendTimeSync(uint16_t year, uint8_t month, uint8_t date,
                             uint8_t hour, uint8_t minute, uint8_t second, uint32_t epoch);

    static uint8_t calcChecksum(const char *payload);

    static void setOnSnapshotReceived(SnapshotCallback cb);
    static void setOnRelayAck(RelayAckCallback cb);
    static void setOnPendingEmpty(PendingEmptyCallback cb);

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
    static void parseGoodWeGroup(const char *group, char *args[], size_t count);
    static void parseRelay(char *args[], size_t count);
};
