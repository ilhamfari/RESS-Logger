/**
  * @file    relay.h
  * @brief   Four relay outputs: RELAY1..RELAY4 = PE7..PE10, active HIGH
  *          (pins are configured by CubeMX in gpio.c). All relays start OFF.
  */

#ifndef RELAY_H
#define RELAY_H

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Relay_Init(void);

/** channel 1..4, state 0/1. Returns false for an invalid channel. */
bool Relay_Set(uint8_t channel, bool on);

/** channel 1..4. Reads back the output latch. */
bool Relay_Get(uint8_t channel);

/* Persistent relay state: last sector of the W25Q16 (sector 511). */
#define RELAY_FLASH_ADDR    0x1FF000UL
#define RELAY_FLASH_MAGIC   0x52454C59UL  /* "RELY" */

typedef struct
{
    uint32_t magic;       /* RELAY_FLASH_MAGIC */
    uint8_t  r1;          /* 0 or 1 */
    uint8_t  r2;
    uint8_t  r3;
    uint8_t  r4;
    uint8_t  checksum;    /* XOR of r1..r4 */
    uint8_t  reserved[3];
} RelayFlashConfig_t;

/** Write the current relay states to flash (skipped if unchanged). */
bool Relay_SaveToFlash(void);

/**
  * @brief Read the saved states and apply them to PE7..PE10. If the flash
  *        is empty/invalid, all relays are forced OFF. Call after
  *        MX_SPI3_Init() and Relay_Init(). Returns true if a valid saved
  *        state was applied.
  */
bool Relay_RestoreFromFlash(void);

/** Relay_Set() followed by Relay_SaveToFlash() when the command succeeded. */
bool Relay_SetAndSave(uint8_t channel, bool on);

/**
  * @brief Compare flash content with the current outputs.
  * @retval 1 valid saved state (mask in *saved_mask), 0 blank/invalid,
  *         -1 flash read error.
  */
int Relay_FlashStatus(uint8_t *saved_mask);

/** Bit n-1 = relay n, for logging. */
uint8_t Relay_GetMask(void);

#ifdef __cplusplus
}
#endif

#endif /* RELAY_H */
