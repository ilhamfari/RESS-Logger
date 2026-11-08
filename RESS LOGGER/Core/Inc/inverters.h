/**
  * @file    inverters.h
  * @brief   GoodWe (FC 0x03, 8N2) and Sungrow (FC 0x04, 8N1) inverter
  *          Modbus drivers. Both produce the same Catalog Seed v1 metrics:
  *          pv_voltage [V], pv_current [A], pv_power [W], pv_energy_total
  *          [kWh], inv_power_out [W], load_power [W], temp_internal [degC],
  *          inv_status [text]. Each metric group has its own validity flag
  *          so a failed or unsupported register block shows up as "NA"
  *          instead of a wrong number.
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

void GoodWe_Init(uint8_t slave_id, uint32_t poll_interval_ms);
void Sungrow_Init(uint8_t slave_id, uint32_t poll_interval_ms);

/** Copy out latest data; .online reflects the Modbus device state. */
void GoodWe_GetLatest(Inverter_Data_t *out);
void Sungrow_GetLatest(Inverter_Data_t *out);

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
