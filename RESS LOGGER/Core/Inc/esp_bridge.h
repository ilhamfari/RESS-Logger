/**
  * @file    esp_bridge.h
  * @brief   STM32 <-> ESP32-S3 link on USART3 (115200 8N1).
  *          Frames: '$' <payload> '*' <XX> "\r\n", XX = two hex digits,
  *          XOR of every character between '$' and '*'.
  *
  *   ESP32 -> STM32:  $REQ,TELE,<epoch>    request a scheduled telemetry batch
  *                    $CMD,RELAY,<ch>,<s>  set relay ch(1..4) to s(0/1)
  *                    $ACK,TELE,<seq>,SUCCESS  MQTT PUBACK received
  *   STM32 -> ESP32:  $DATA,LARKIN|GOODWE|SUNGROW|RELAY|END,<seq>,...
  *                    $ACK,RELAY,<ch>,<s>
  *                    $ERR,<what>,<reason>
  *
  *   RX is interrupt driven (1 byte at a time into a ring buffer), TX is a
  *   short blocking transmit (a frame is ~10 ms at 115200).
  */

#ifndef ESP_BRIDGE_H
#define ESP_BRIDGE_H

#include "main.h"
#include "ress_snapshot.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    /** Request one snapshot for the scheduled UTC epoch.  A value of zero is
     *  accepted for compatibility with older ESP32 firmware. */
    void (*on_req_tele)(uint32_t scheduled_epoch);
    /** Apply a relay command; return the resulting state, or -1 if invalid. */
    int  (*on_cmd_relay)(uint8_t channel, uint8_t state);
    void (*on_ack_tele)(uint32_t seq, bool success);
    /** Apply time synchronization; return true if accepted. */
    bool (*on_cmd_time)(uint16_t year, uint8_t month, uint8_t date,
                        uint8_t hour, uint8_t minute, uint8_t second, uint32_t epoch);
} ESP_Handlers_t;

void ESP_Bridge_Init(UART_HandleTypeDef *huart, const ESP_Handlers_t *handlers);

/** Main-loop hook: assemble received bytes into frames and dispatch them. */
void ESP_Bridge_Process(void);

/** Ask the ESP32 for the NTP time ($REQ,TIME); it replies with $CMD,TIME. */
bool ESP_Bridge_SendReqTime(void);

/** Send the full $DATA batch (LARKIN, GOODWE, SUNGROW, RELAY, END). */
bool ESP_Bridge_SendSnapshot(const RessSnapshot_t *snap);

#ifdef __cplusplus
}
#endif

#endif /* ESP_BRIDGE_H */
