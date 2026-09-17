/**
  * @file    at24c256.h
  * @brief   Driver for Microchip AT24C256 32KB I2C EEPROM (common breakout
  *          module, A0/A1/A2 tied high on this board's module -> address
  *          0x57 -- confirmed via I2C bus scan on hardware), on shared
  *          I2C1 bus.
  */

#ifndef AT24C256_H
#define AT24C256_H

#include "main.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT24C256_ADDR         (0x57U << 1)
#define AT24C256_SIZE_BYTES   32768U
#define AT24C256_PAGE_SIZE    64U

typedef struct
{
    I2C_HandleTypeDef *hi2c;
} AT24C256_Handle_t;

void AT24C256_Init(AT24C256_Handle_t *dev, I2C_HandleTypeDef *hi2c);

/**
  * @brief Sequential read, any length/offset -- no page constraints on reads.
  */
HAL_StatusTypeDef AT24C256_Read(AT24C256_Handle_t *dev, uint16_t mem_addr, uint8_t *data, uint16_t len);

/**
  * @brief Write, automatically split across AT24C256_PAGE_SIZE page
  *        boundaries (writes that cross a page boundary wrap within the
  *        page on real EEPROMs if done as one transfer, so this function
  *        chunks them). Blocks ~5ms per page for the internal write cycle.
  */
HAL_StatusTypeDef AT24C256_Write(AT24C256_Handle_t *dev, uint16_t mem_addr, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* AT24C256_H */
