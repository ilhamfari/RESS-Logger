/**
  * @file    power_meter.h
  * @brief   Multifunction network power meter (Modbus slave, default 0x01):
  *          register block 0x0023..0x003E (28 registers) decoding and
  *          engineering-unit scaling (DPT/DCT/DPQ/SIGN).
  */

#ifndef POWER_METER_H
#define POWER_METER_H

#include "modbus_master.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PM_START_REG   0x0023U
#define PM_REG_COUNT   28U

typedef struct
{
    float ua, ub, uc;          /* V  */
    float uab, ubc, uca;       /* V  */
    float ia, ib, ic;          /* A  */
    float pa, pb, pc, p_total; /* W   (signed) */
    float qa, qb, qc, q_total; /* var (signed) */
    float sa, sb, sc, s_total; /* VA  */
    float pf1, pf2, pf3, pf_avg;
    float freq;                /* Hz */

    uint8_t dpt, dct, dpq, sign; /* raw scaling registers, kept for debug */
    uint32_t update_tick;        /* HAL tick of the last good decode */
    bool valid;                  /* true after the first good decode */
} PowerMeter_Data_t;

/** Register the meter in the Modbus polling table. */
void PowerMeter_Init(uint8_t slave_id, uint32_t poll_interval_ms);

/**
  * @brief Copy out the latest decoded values.
  * @retval true if the meter is currently online and data is valid.
  */
bool PowerMeter_GetLatest(PowerMeter_Data_t *out);

bool PowerMeter_IsOnline(void);

/** Decode a 56-byte big-endian register block. false if scaling is invalid. */
bool PowerMeter_Decode(const uint8_t *data, uint16_t len, PowerMeter_Data_t *out);

/**
  * @brief Write the 26 measurement fields as CSV text, in the agreed column
  *        order: ua..uca, ia..ic, pa..p_total, qa..q_total, sa..s_total,
  *        pf1..pf_avg, freq. No leading/trailing comma.
  */
int PowerMeter_FormatFields(char *buf, size_t size, const PowerMeter_Data_t *d);

/** Multi-line human readable dump on the serial monitor. */
void PowerMeter_PrintDebug(const PowerMeter_Data_t *d);

#ifdef __cplusplus
}
#endif

#endif /* POWER_METER_H */
