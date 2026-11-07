#include "modbus_master.h"
#include "iwdg.h"
#include "ress_util.h"
#include <string.h>

/* Set to 1 to dump every TX/RX frame as hex on the serial monitor. */
#ifndef MB_DEBUG_HEX
#define MB_DEBUG_HEX 0
#endif

static UART_HandleTypeDef *s_huart;
static ModbusDevice_t     *s_devices[MB_MAX_DEVICES];
static uint8_t             s_device_count;
static uint8_t             s_poll_cursor;

static uint8_t s_rx[5U + 2U * MB_MAX_REGS];
static uint8_t s_data[2U * MB_MAX_REGS];

uint16_t Modbus_CRC16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8U; b++)
        {
            if ((crc & 0x0001U) != 0U)
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

const char *Modbus_StatusStr(ModbusStatus_t st)
{
    switch (st)
    {
    case MB_OK:            return "OK";
    case MB_ERR_PARAM:     return "bad parameter";
    case MB_ERR_UART:      return "UART transmit error";
    case MB_ERR_TIMEOUT:   return "timeout (no response)";
    case MB_ERR_FRAME:     return "short/malformed frame";
    case MB_ERR_CRC:       return "CRC error";
    case MB_ERR_EXCEPTION: return "Modbus exception";
    default:               return "unknown";
    }
}

static void HexDump(const char *tag, const uint8_t *p, uint16_t n)
{
    char line[3U * 24U + 1U];
    uint16_t pos = 0;

    printf("[%8lu] MODBUS  %s (%u bytes):", (unsigned long)HAL_GetTick(), tag, (unsigned)n);
    for (uint16_t i = 0; i < n; i++)
    {
        pos += (uint16_t)snprintf(&line[pos], sizeof(line) - pos, " %02X", p[i]);
        if (pos >= 3U * 20U || i == (uint16_t)(n - 1U))
        {
            printf("%s", line);
            pos = 0;
        }
    }
    printf("\r\n");
}

static void FlushRx(void)
{
    /* Reading DR also clears ORE/FE/NE/PE. */
    while (__HAL_UART_GET_FLAG(s_huart, UART_FLAG_RXNE) != RESET)
    {
        (void)s_huart->Instance->DR;
    }
    __HAL_UART_CLEAR_OREFLAG(s_huart);
}

void Modbus_Init(UART_HandleTypeDef *huart)
{
    s_huart = huart;
    s_device_count = 0;
    s_poll_cursor = 0;
    RESS_LOG("MODBUS", "init: USART1 baud=%lu, 8E1, timeout=%u ms",
             (unsigned long)huart->Init.BaudRate, (unsigned)MB_RESPONSE_TIMEOUT_MS);
}

bool Modbus_RegisterDevice(ModbusDevice_t *dev)
{
    if (dev == NULL || s_device_count >= MB_MAX_DEVICES)
    {
        RESS_LOG("MODBUS", "register FAILED (table full or NULL)");
        return false;
    }

    dev->is_online = false;
    dev->has_polled = false;
    dev->fail_streak = 0;
    dev->ok_count = 0;
    dev->err_count = 0;
    s_devices[s_device_count++] = dev;

    RESS_LOG("MODBUS", "registered '%s' slave=0x%02X regs=0x%04X..+%u every %lu ms",
             dev->name, dev->slave_id, dev->start_reg, (unsigned)dev->reg_count,
             (unsigned long)dev->poll_interval_ms);
    return true;
}

ModbusStatus_t Modbus_ReadHoldingRegisters(uint8_t slave, uint16_t start,
                                           uint16_t count, uint8_t *out,
                                           uint8_t *exc_code)
{
    uint8_t tx[8];

    if (s_huart == NULL || out == NULL || count == 0U || count > MB_MAX_REGS)
    {
        return MB_ERR_PARAM;
    }

    tx[0] = slave;
    tx[1] = 0x03U;
    tx[2] = (uint8_t)(start >> 8);
    tx[3] = (uint8_t)(start & 0xFFU);
    tx[4] = (uint8_t)(count >> 8);
    tx[5] = (uint8_t)(count & 0xFFU);
    uint16_t crc = Modbus_CRC16(tx, 6);
    tx[6] = (uint8_t)(crc & 0xFFU);   /* CRC low byte first */
    tx[7] = (uint8_t)(crc >> 8);

    HAL_Delay(MB_INTERFRAME_MS);
    FlushRx();

#if MB_DEBUG_HEX
    HexDump("TX", tx, sizeof(tx));
#endif

    if (HAL_UART_Transmit(s_huart, tx, sizeof(tx), 100U) != HAL_OK)
    {
        return MB_ERR_UART;
    }

    uint16_t expected = (uint16_t)(5U + 2U * count);
    HAL_StatusTypeDef hs = HAL_UART_Receive(s_huart, s_rx, expected, MB_RESPONSE_TIMEOUT_MS);
    HAL_IWDG_Refresh(&hiwdg);

    uint16_t got = (hs == HAL_OK) ? expected : (uint16_t)(expected - s_huart->RxXferCount);

#if MB_DEBUG_HEX
    HexDump("RX", s_rx, got);
#endif

    /* Exception frame: slave | 0x83 | code | crc16 (5 bytes). */
    if (got >= 5U && s_rx[0] == slave && s_rx[1] == (uint8_t)(0x80U | 0x03U))
    {
        uint16_t ecrc = Modbus_CRC16(s_rx, 3);
        if (s_rx[3] == (uint8_t)(ecrc & 0xFFU) && s_rx[4] == (uint8_t)(ecrc >> 8))
        {
            if (exc_code != NULL)
            {
                *exc_code = s_rx[2];
            }
            return MB_ERR_EXCEPTION;
        }
    }

    if (hs != HAL_OK)
    {
        if (got == 0U)
        {
            return MB_ERR_TIMEOUT;
        }
        HexDump("partial RX", s_rx, got);
        return MB_ERR_FRAME;
    }

    uint16_t rcrc = Modbus_CRC16(s_rx, (uint16_t)(expected - 2U));
    if (s_rx[expected - 2U] != (uint8_t)(rcrc & 0xFFU) ||
        s_rx[expected - 1U] != (uint8_t)(rcrc >> 8))
    {
        HexDump("bad-CRC RX", s_rx, expected);
        return MB_ERR_CRC;
    }

    if (s_rx[0] != slave || s_rx[1] != 0x03U || s_rx[2] != (uint8_t)(2U * count))
    {
        HexDump("bad-header RX", s_rx, expected);
        return MB_ERR_FRAME;
    }

    memcpy(out, &s_rx[3], 2U * count);
    return MB_OK;
}

static void PollDevice(ModbusDevice_t *dev)
{
    uint8_t exc = 0;
    uint32_t t0 = HAL_GetTick();

    ModbusStatus_t st = Modbus_ReadHoldingRegisters(dev->slave_id, dev->start_reg,
                                                    dev->reg_count, s_data, &exc);
    uint32_t dt = HAL_GetTick() - t0;
    bool ok = false;

    if (st == MB_OK)
    {
        ok = (dev->parse == NULL) ? true : dev->parse(dev, s_data, (uint16_t)(2U * dev->reg_count));
        if (!ok)
        {
            RESS_LOG("MODBUS", "%s: payload rejected by parser", dev->name);
        }
    }
    else if (st == MB_ERR_EXCEPTION)
    {
        RESS_LOG("MODBUS", "%s: exception code 0x%02X", dev->name, exc);
    }
    else
    {
        RESS_LOG("MODBUS", "%s: poll FAILED -> %s (%lu ms)", dev->name,
                 Modbus_StatusStr(st), (unsigned long)dt);
    }

    if (ok)
    {
        dev->ok_count++;
        dev->fail_streak = 0;
        if (!dev->is_online)
        {
            RESS_LOG("MODBUS", "%s: now ONLINE", dev->name);
        }
        dev->is_online = true;
    }
    else
    {
        dev->err_count++;
        if (dev->fail_streak < 255U)
        {
            dev->fail_streak++;
        }
        if (dev->is_online && dev->fail_streak >= MB_OFFLINE_THRESHOLD)
        {
            dev->is_online = false;
            RESS_LOG("MODBUS", "%s: now OFFLINE after %u failures", dev->name,
                     (unsigned)dev->fail_streak);
        }
    }
}

void Modbus_Poll(void)
{
    uint32_t now = HAL_GetTick();

    for (uint8_t n = 0; n < s_device_count; n++)
    {
        uint8_t idx = (uint8_t)((s_poll_cursor + n) % s_device_count);
        ModbusDevice_t *dev = s_devices[idx];

        if (!dev->has_polled || (now - dev->last_poll_tick) >= dev->poll_interval_ms)
        {
            dev->has_polled = true;
            dev->last_poll_tick = now;
            s_poll_cursor = (uint8_t)((idx + 1U) % s_device_count);
            PollDevice(dev);
            return; /* one device per call keeps the main loop responsive */
        }
    }
}
