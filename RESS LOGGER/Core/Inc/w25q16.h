/**
  * @file    w25q16.h
  * @brief   Driver for Winbond W25Q-family SPI NOR flash (Devebox onboard
  *          W25Q16, 2MB) on shared SPI3 bus (CS=PA15/FLASH_CS). SPI3 is
  *          also used by W5500 #2 (CS=PD4) -- callers on that bus must not
  *          assert this device's CS while another device's CS is asserted.
  *
  *          Command set, page size (256B) and sector size (4KB) are the
  *          same across the W25Q family (W25Q16/32/64/128/...), and 3-byte
  *          addressing covers up to 16MB, so this driver also works
  *          unmodified on a W25Q128 -- only W25Q16_SIZE_BYTES needs to
  *          change (JEDEC ID check is informational, not enforced).
  */

#ifndef W25Q16_H
#define W25Q16_H

#include "main.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define W25Q16_SIZE_BYTES     (2U * 1024U * 1024U)   /* 2MB; set to 16MB for W25Q128 */
#define W25Q16_PAGE_SIZE      256U
#define W25Q16_SECTOR_SIZE    4096U

#define W25Q16_JEDEC_MANUFACTURER  0xEFU  /* Winbond */
#define W25Q16_JEDEC_MEMTYPE       0x40U
#define W25Q16_JEDEC_CAPACITY      0x15U  /* W25Q128 = 0x18 */

typedef struct
{
    SPI_HandleTypeDef *hspi;
    GPIO_TypeDef       *cs_port;
    uint16_t            cs_pin;
} W25Q16_Handle_t;

/**
  * @brief Latch the SPI handle/CS pin and read back the JEDEC ID (logged by
  *        the caller if desired via W25Q16_ReadJedecId) -- does not itself
  *        fail if the ID doesn't match W25Q16_JEDEC_*, since this driver is
  *        meant to also run against a W25Q128.
  */
void W25Q16_Init(W25Q16_Handle_t *dev, SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin);

/**
  * @brief Read the 3-byte JEDEC ID (manufacturer, memory type, capacity).
  */
HAL_StatusTypeDef W25Q16_ReadJedecId(W25Q16_Handle_t *dev, uint8_t id[3]);

/**
  * @brief Sequential read, any length/offset -- no page constraints on reads.
  */
HAL_StatusTypeDef W25Q16_Read(W25Q16_Handle_t *dev, uint32_t addr, uint8_t *data, uint32_t len);

/**
  * @brief Write, automatically split across W25Q16_PAGE_SIZE page
  *        boundaries (a write crossing a page boundary wraps within the
  *        page on real NOR flash if done as one transfer, so this chunks
  *        them). Target bytes must already be erased (0xFF) -- this only
  *        clears bits, it does not erase. Blocks for each page's internal
  *        write cycle.
  */
HAL_StatusTypeDef W25Q16_Write(W25Q16_Handle_t *dev, uint32_t addr, const uint8_t *data, uint32_t len);

/**
  * @brief Erase one 4KB sector containing addr (addr is rounded down to the
  *        sector boundary internally). Blocks for the erase cycle.
  */
HAL_StatusTypeDef W25Q16_EraseSector(W25Q16_Handle_t *dev, uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* W25Q16_H */
