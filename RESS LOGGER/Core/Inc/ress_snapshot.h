/**
  * @file    ress_snapshot.h
  * @brief   One consistent copy of all device data, taken at a single
  *          instant. It is what gets written to the SD card (one CSV row)
  *          and what is sent to the ESP32 for a given sequence number.
  */

#ifndef RESS_SNAPSHOT_H
#define RESS_SNAPSHOT_H

#include "ds3231.h"
#include "inverters.h"
#include "power_meter.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    bool              valid;
    uint32_t          seq;
    DS3231_Time_t     time;
    bool              time_valid;
    bool              meter_online;
    PowerMeter_Data_t meter;
    Inverter_Data_t   goodwe;
    Inverter_Data_t   sungrow;
    uint8_t           relays;  /* bit n-1 = relay n */
    bool              sent;    /* ESP32 confirmed MQTT delivery */
} RessSnapshot_t;

#endif /* RESS_SNAPSHOT_H */
