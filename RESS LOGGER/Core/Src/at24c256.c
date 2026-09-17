#include "at24c256.h"

#define AT24C256_I2C_TIMEOUT_MS   100U
#define AT24C256_WRITE_CYCLE_MS   5U /* max internal write cycle time, datasheet */

void AT24C256_Init(AT24C256_Handle_t *dev, I2C_HandleTypeDef *hi2c)
{
    dev->hi2c = hi2c;
}

HAL_StatusTypeDef AT24C256_Read(AT24C256_Handle_t *dev, uint16_t mem_addr, uint8_t *data, uint16_t len)
{
    return HAL_I2C_Mem_Read(dev->hi2c, AT24C256_ADDR, mem_addr,
                             I2C_MEMADD_SIZE_16BIT, data, len, AT24C256_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef AT24C256_Write(AT24C256_Handle_t *dev, uint16_t mem_addr, const uint8_t *data, uint16_t len)
{
    while (len > 0U)
    {
        uint16_t offset_in_page = mem_addr % AT24C256_PAGE_SIZE;
        uint16_t space_in_page = AT24C256_PAGE_SIZE - offset_in_page;
        uint16_t chunk = (len < space_in_page) ? len : space_in_page;

        HAL_StatusTypeDef status = HAL_I2C_Mem_Write(dev->hi2c, AT24C256_ADDR, mem_addr,
                                                       I2C_MEMADD_SIZE_16BIT,
                                                       (uint8_t *)data, chunk, AT24C256_I2C_TIMEOUT_MS);
        if (status != HAL_OK)
        {
            return status;
        }

        /* EEPROM is unresponsive on the bus during the internal write
         * cycle; a fixed delay is simplest here (ACK polling is a later
         * optimization if this blocking time matters for the app). */
        HAL_Delay(AT24C256_WRITE_CYCLE_MS);

        mem_addr += chunk;
        data += chunk;
        len -= chunk;
    }

    return HAL_OK;
}
