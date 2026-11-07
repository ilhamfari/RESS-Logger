/**
  * @file    esp_bridge.h
  * @brief   STM32 -> ESP32-S3 telemetry link on USART3 (115200 8N1,
  *          interrupt driven TX). Frames are ASCII, '$' ... '*XX\n', where
  *          XX is the XOR of all characters between '$' and '*' in hex:
  *
  *   $TELE,RESS01,2026-11-07 08:50:00,<26 fields>*XX\n
  *   $STAT,RESS01,OFFLINE*XX\n
  *
  *   The timestamp is 0000-00-00 00:00:00 when the RTC time is not valid.
  */

#ifndef ESP_BRIDGE_H
#define ESP_BRIDGE_H

#include "main.h"
#include "power_meter.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ESP_Bridge_Init(UART_HandleTypeDef *huart);

/** Returns false if the previous frame is still being transmitted. */
bool ESP_Bridge_SendTelemetry(const char *timestamp, const PowerMeter_Data_t *d);

bool ESP_Bridge_SendStatus(bool meter_online);

#ifdef __cplusplus
}
#endif

#endif /* ESP_BRIDGE_H */
