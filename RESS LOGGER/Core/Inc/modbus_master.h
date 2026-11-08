/**
  * @file    modbus_master.h
  * @brief   Modbus RTU master (function 0x03 Read Holding Registers) on
  *          USART1 plus a small multi-device polling table. The table is
  *          what lets a solar inverter (slave 0x02) be added later without
  *          touching the engine: register another ModbusDevice_t with its
  *          own register block and parse callback.
  */

#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB_MAX_REGS            125U   /* Modbus limit for FC 0x03 */
#define MB_MAX_DEVICES         4U
#define MB_RESPONSE_TIMEOUT_MS 200U   /* per plan: meter must answer in 200 ms */
#define MB_INTERFRAME_MS       5U     /* > 3.5 chars at 9600 bps (~4 ms) */
#define MB_OFFLINE_THRESHOLD   3U     /* consecutive failures before "offline" */

typedef enum
{
    MB_OK = 0,
    MB_ERR_PARAM,
    MB_ERR_UART,       /* HAL transmit failed */
    MB_ERR_TIMEOUT,    /* no byte received at all */
    MB_ERR_FRAME,      /* partial / malformed response */
    MB_ERR_CRC,
    MB_ERR_EXCEPTION   /* slave returned a Modbus exception */
} ModbusStatus_t;

typedef enum
{
    DEVICE_TYPE_POWER_METER = 0,
    DEVICE_TYPE_INVERTER
} ModbusDeviceType_t;

/** Serial format; the meter is 8E1, the inverters are 8N1 on the same bus. */
typedef enum
{
    MB_FMT_8N1 = 0,
    MB_FMT_8E1
} ModbusFormat_t;

#define MB_FC_READ_HOLDING  0x03U
#define MB_FC_READ_INPUT    0x04U

struct ModbusDevice_s;

/**
  * Device poll routine: performs all register reads the device needs (via
  * Modbus_ReadRegisters) and stores the decoded result. Return true if at
  * least the essential data was obtained (counts as a successful poll).
  */
typedef bool (*ModbusPollFn_t)(const struct ModbusDevice_s *dev);

typedef struct ModbusDevice_s
{
    const char        *name;
    uint8_t            slave_id;
    ModbusDeviceType_t dev_type;
    ModbusFormat_t     format;
    uint32_t           poll_interval_ms;
    uint32_t           offline_retry_ms; /* poll period while offline (0 = same) */
    uint32_t           last_poll_tick;
    bool               is_online;
    ModbusPollFn_t     poll;
    /* runtime statistics */
    bool               has_polled;
    uint8_t            fail_streak;
    uint32_t           ok_count;
    uint32_t           err_count;
} ModbusDevice_t;

uint16_t Modbus_CRC16(const uint8_t *data, uint16_t len);

void Modbus_Init(UART_HandleTypeDef *huart);

/** Register a device in the polling table. Returns false if table full. */
bool Modbus_RegisterDevice(ModbusDevice_t *dev);

/** Switch USART1 between 8N1 and 8E1 (no-op if already set). */
void Modbus_SetFormat(ModbusFormat_t fmt);

/**
  * @brief Single blocking read transaction (FC 0x03 or 0x04).
  * @param out       receives 2*count bytes (big-endian register data)
  * @param exc_code  receives the exception code on MB_ERR_EXCEPTION (may be NULL)
  */
ModbusStatus_t Modbus_ReadRegisters(uint8_t slave, uint8_t fc, uint16_t start,
                                    uint16_t count, uint8_t *out,
                                    uint8_t *exc_code);

/** Big-endian register helpers for poll routines. */
uint16_t Modbus_U16(const uint8_t *data, uint16_t reg_index);

/** Call from the main loop; polls at most one due device per call. */
void Modbus_Poll(void);

const char *Modbus_StatusStr(ModbusStatus_t st);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_MASTER_H */
