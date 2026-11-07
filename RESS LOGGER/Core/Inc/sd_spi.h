/**
  * @file    sd_spi.h
  * @brief   SD/SDHC/SDXC block driver over SPI2 (SCK=PB10, MISO=PC2,
  *          MOSI=PC3, CS=PA0/SD_CS). Implements the MMC/SD-over-SPI
  *          command subset needed to back FatFs's "User-defined" disk I/O
  *          driver (FATFS/Target/user_diskio.c) -- card detect/init,
  *          single- and multi-block read/write, and the capacity query
  *          FatFs needs at f_mount time.
  */

#ifndef SD_SPI_H
#define SD_SPI_H

#include "main.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_SPI_BLOCK_SIZE   512U

/**
  * @brief Card detect + full init sequence (>=74 dummy clocks, CMD0, CMD8,
  *        ACMD41 polling, CMD58 OCR/CCS check, CMD16 for SDSC). Switches
  *        SPI2 to a slow clock for the handshake, then to its normal
  *        (fast) clock once the card is in data-transfer state.
  * @retval 1 on success, 0 on failure (no card / init timeout / protocol
  *         error).
  */
uint8_t SD_SPI_Init(void);

/**
  * @brief Whether SD_SPI_Init() has completed successfully. disk_read/
  *        disk_write/ioctl calls before this is true will fail.
  */
uint8_t SD_SPI_IsInitialized(void);

/**
  * @brief Read `count` consecutive 512-byte blocks starting at `sector`
  *        into `buff` (buff must hold count*512 bytes).
  * @retval 1 on success, 0 on failure.
  */
uint8_t SD_SPI_ReadBlocks(uint8_t *buff, uint32_t sector, uint32_t count);

/**
  * @brief Write `count` consecutive 512-byte blocks starting at `sector`
  *        from `buff`.
  * @retval 1 on success, 0 on failure.
  */
uint8_t SD_SPI_WriteBlocks(const uint8_t *buff, uint32_t sector, uint32_t count);

/**
  * @brief Total card capacity in 512-byte sectors (parsed from the CSD
  *        register during SD_SPI_Init()). 0 if not initialized.
  */
uint32_t SD_SPI_GetSectorCount(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_SPI_H */
