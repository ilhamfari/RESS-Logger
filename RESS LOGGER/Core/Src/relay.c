#include "relay.h"
#include "ress_config.h"
#include "ress_util.h"
#include "w25q16.h"
#include "spi.h"
#include "iwdg.h"
#include <string.h>

static GPIO_TypeDef *const s_port[RESS_RELAY_COUNT] =
    { RELAY1_GPIO_Port, RELAY2_GPIO_Port, RELAY3_GPIO_Port, RELAY4_GPIO_Port };
static const uint16_t s_pin[RESS_RELAY_COUNT] =
    { RELAY1_Pin, RELAY2_Pin, RELAY3_Pin, RELAY4_Pin };

void Relay_Init(void)
{
    for (uint8_t i = 0; i < RESS_RELAY_COUNT; i++)
    {
        HAL_GPIO_WritePin(s_port[i], s_pin[i], GPIO_PIN_RESET);
    }
    RESS_LOG("RELAY", "init: PE7..PE10 all OFF");
}

bool Relay_Set(uint8_t channel, bool on)
{
    if (channel < 1U || channel > RESS_RELAY_COUNT)
    {
        RESS_LOG("RELAY", "invalid channel %u", (unsigned)channel);
        return false;
    }

    HAL_GPIO_WritePin(s_port[channel - 1U], s_pin[channel - 1U], on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    RESS_LOG("RELAY", "relay %u -> %s", (unsigned)channel, on ? "ON" : "OFF");
    return true;
}

bool Relay_Get(uint8_t channel)
{
    if (channel < 1U || channel > RESS_RELAY_COUNT)
    {
        return false;
    }
    return HAL_GPIO_ReadPin(s_port[channel - 1U], s_pin[channel - 1U]) == GPIO_PIN_SET;
}

uint8_t Relay_GetMask(void)
{
    uint8_t m = 0;
    for (uint8_t i = 1; i <= RESS_RELAY_COUNT; i++)
    {
        if (Relay_Get(i))
        {
            m |= (uint8_t)(1U << (i - 1U));
        }
    }
    return m;
}

/* ------------------------------------------------------------------------- */
/* Persistence in W25Q16 sector 511                                           */
/* ------------------------------------------------------------------------- */

static W25Q16_Handle_t s_flash;

static uint8_t CfgChecksum(const RelayFlashConfig_t *c)
{
    return (uint8_t)(c->r1 ^ c->r2 ^ c->r3 ^ c->r4);
}

static bool CfgValid(const RelayFlashConfig_t *c)
{
    return c->magic == RELAY_FLASH_MAGIC && c->checksum == CfgChecksum(c) &&
           c->r1 <= 1U && c->r2 <= 1U && c->r3 <= 1U && c->r4 <= 1U;
}

bool Relay_SaveToFlash(void)
{
    RelayFlashConfig_t cur, old;
    uint8_t mask = Relay_GetMask();

    memset(&cur, 0xFF, sizeof(cur));
    cur.magic = RELAY_FLASH_MAGIC;
    cur.r1 = (uint8_t)(mask & 1U);
    cur.r2 = (uint8_t)((mask >> 1) & 1U);
    cur.r3 = (uint8_t)((mask >> 2) & 1U);
    cur.r4 = (uint8_t)((mask >> 3) & 1U);
    cur.checksum = CfgChecksum(&cur);
    memset(cur.reserved, 0, sizeof(cur.reserved));

    W25Q16_Init(&s_flash, &hspi3, FLASH_CS_GPIO_Port, FLASH_CS_Pin);

    /* Skip the erase if the stored state already matches (flash wear). */
    if (W25Q16_Read(&s_flash, RELAY_FLASH_ADDR, (uint8_t *)&old, sizeof(old)) == HAL_OK &&
        memcmp(&old, &cur, sizeof(cur)) == 0)
    {
        RESS_LOG("RELAY", "flash already up to date (mask=0x%X)", (unsigned)mask);
        return true;
    }

    /* A sector erase can take hundreds of ms; the watchdog is ~0.5 s. */
    HAL_IWDG_Refresh(&hiwdg);
    if (W25Q16_EraseSector(&s_flash, RELAY_FLASH_ADDR) != HAL_OK)
    {
        RESS_LOG("RELAY", "flash save FAILED (erase)");
        return false;
    }
    HAL_IWDG_Refresh(&hiwdg);
    if (W25Q16_Write(&s_flash, RELAY_FLASH_ADDR, (const uint8_t *)&cur, sizeof(cur)) != HAL_OK)
    {
        RESS_LOG("RELAY", "flash save FAILED (write)");
        return false;
    }
    HAL_IWDG_Refresh(&hiwdg);

    RESS_LOG("RELAY", "saved to flash 0x%06lX: r1=%u r2=%u r3=%u r4=%u", (unsigned long)RELAY_FLASH_ADDR,
             cur.r1, cur.r2, cur.r3, cur.r4);
    return true;
}

bool Relay_RestoreFromFlash(void)
{
    RelayFlashConfig_t c;

    W25Q16_Init(&s_flash, &hspi3, FLASH_CS_GPIO_Port, FLASH_CS_Pin);

    if (W25Q16_Read(&s_flash, RELAY_FLASH_ADDR, (uint8_t *)&c, sizeof(c)) != HAL_OK)
    {
        RESS_LOG("RELAY", "restore: flash read FAILED, all OFF");
        memset(&c, 0, sizeof(c));
    }
    else if (!CfgValid(&c))
    {
        RESS_LOG("RELAY", "restore: no valid saved state (magic=0x%08lX), all OFF", (unsigned long)c.magic);
        memset(&c, 0, sizeof(c));
        for (uint8_t i = 1; i <= RESS_RELAY_COUNT; i++)
        {
            (void)Relay_Set(i, false);
        }
        return false;
    }

    (void)Relay_Set(1, c.r1 != 0U);
    (void)Relay_Set(2, c.r2 != 0U);
    (void)Relay_Set(3, c.r3 != 0U);
    (void)Relay_Set(4, c.r4 != 0U);
    RESS_LOG("RELAY", "restored from flash: r1=%u r2=%u r3=%u r4=%u", c.r1, c.r2, c.r3, c.r4);
    return (c.magic == RELAY_FLASH_MAGIC);
}

bool Relay_SetAndSave(uint8_t channel, bool on)
{
    if (!Relay_Set(channel, on))
    {
        return false;
    }
    (void)Relay_SaveToFlash(); /* relay is already switched even if saving fails */
    return true;
}

int Relay_FlashStatus(uint8_t *saved_mask)
{
    RelayFlashConfig_t c;

    W25Q16_Init(&s_flash, &hspi3, FLASH_CS_GPIO_Port, FLASH_CS_Pin);
    if (W25Q16_Read(&s_flash, RELAY_FLASH_ADDR, (uint8_t *)&c, sizeof(c)) != HAL_OK)
    {
        return -1;
    }
    if (!CfgValid(&c))
    {
        return 0;
    }
    if (saved_mask != NULL)
    {
        *saved_mask = (uint8_t)(c.r1 | (c.r2 << 1) | (c.r3 << 2) | (c.r4 << 3));
    }
    return 1;
}
