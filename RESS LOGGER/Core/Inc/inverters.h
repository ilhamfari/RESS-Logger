/**
  * @file    inverters.h
  * @brief   GoodWe GW10KT-DT (FC 0x03, 8N2) telemetry and legacy Sungrow
  *          support. GoodWe fields carry individual validity flags so CSV
  *          can write NA and MQTT can publish null for unavailable values.
  */

#ifndef INVERTERS_H
#define INVERTERS_H

#include "modbus_master.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool  online;
    bool  ok_pv;      /* pv_voltage, pv_current */
    bool  ok_ppv;     /* pv_power */
    bool  ok_energy;  /* pv_energy_total */
    bool  ok_pout;    /* inv_power_out */
    bool  ok_load;    /* load_power */
    bool  ok_temp;    /* temp_internal */
    bool  ok_status;  /* inv_status */
    float vpv;        /* V */
    float ipv;        /* A */
    float ppv;        /* W */
    float energy_kwh; /* cumulative kWh */
    float pout;       /* W */
    float pload;      /* W */
    float temp;       /* degC */
    const char *status;
    uint32_t update_tick;
} Inverter_Data_t;

typedef struct
{
    bool online;

    bool has_device_serial;
    bool has_device_type;
    char device_serial[17];
    char device_type[17];

    bool has_error_code;
    bool has_operating_hours;
    bool has_working_status;
    bool has_grid_connection_status;
    bool has_inverter_temperature;
    uint32_t error_code;
    uint32_t operating_hours_total;
    char working_status[16];
    char grid_connection_status[16];
    float inverter_temperature;

    bool has_mppt1_voltage;
    bool has_mppt1_current;
    bool has_mppt1_power;
    bool has_mppt2_voltage;
    bool has_mppt2_current;
    bool has_mppt2_power;
    bool has_pv_power_total;
    bool has_insulation_resistance;
    bool has_leakage_current;
    float mppt1_voltage;
    float mppt1_current;
    float mppt1_power;
    float mppt2_voltage;
    float mppt2_current;
    float mppt2_power;
    float pv_power_total;
    float insulation_resistance;
    float leakage_current;

    bool has_active_power;
    bool has_reactive_power;
    bool has_power_factor;
    bool has_phase_a_voltage;
    bool has_phase_b_voltage;
    bool has_phase_c_voltage;
    bool has_phase_a_current;
    bool has_phase_b_current;
    bool has_phase_c_current;
    bool has_phase_a_frequency;
    bool has_phase_b_frequency;
    bool has_phase_c_frequency;
    bool has_grid_frequency;
    bool has_line_ab_voltage;
    bool has_line_bc_voltage;
    bool has_line_ca_voltage;
    float active_power;
    float reactive_power;
    float power_factor;
    float phase_a_voltage;
    float phase_b_voltage;
    float phase_c_voltage;
    float phase_a_current;
    float phase_b_current;
    float phase_c_current;
    float phase_a_frequency;
    float phase_b_frequency;
    float phase_c_frequency;
    float grid_frequency;
    float line_ab_voltage;
    float line_bc_voltage;
    float line_ca_voltage;

    bool has_energy_total;
    bool has_energy_today;
    float energy_total;
    float energy_today;

    uint32_t update_tick;
} GoodWe_Data_t;

void GoodWe_Init(uint8_t slave_id, uint32_t poll_interval_ms);
void GoodWe_SetEnabled(bool enabled);
bool GoodWe_IsEnabled(void);
void Sungrow_Init(uint8_t slave_id, uint32_t poll_interval_ms);

/** Copy out latest data; .online reflects the Modbus device state. */
void GoodWe_GetLatest(GoodWe_Data_t *out);
void Sungrow_GetLatest(Inverter_Data_t *out);

/** Format the complete fixed-order GoodWe field list for CSV/UART. */
int GoodWe_FormatFields(char *buf, size_t size, const GoodWe_Data_t *d);

/** Number of fields emitted by GoodWe_FormatFields(). */
#define GOODWE_FIELD_COUNT 34U

void GoodWe_PrintDebug(const GoodWe_Data_t *d);

/**
  * @brief 8 CSV fields: vpv,ipv,ppv,e_total,p_out,p_load,status,temp.
  *        Invalid metrics (or an offline inverter) are written as "NA".
  */
int Inverter_FormatFields(char *buf, size_t size, const Inverter_Data_t *d);

/** Human readable dump on the serial monitor. */
void Inverter_PrintDebug(const char *name, const Inverter_Data_t *d);

#ifdef __cplusplus
}
#endif

#endif /* INVERTERS_H */
