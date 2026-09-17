#include "ds3231.h"

#define DS3231_I2C_TIMEOUT_MS  50U

/* Register map (DS3231 datasheet Table 1) */
#define DS3231_REG_SECONDS     0x00U
#define DS3231_REG_MINUTES     0x01U
#define DS3231_REG_HOURS       0x02U
#define DS3231_REG_DATE        0x04U
#define DS3231_REG_MONTH       0x05U /* bit7 = century */
#define DS3231_REG_YEAR        0x06U
#define DS3231_REG_CONTROL     0x0EU
#define DS3231_REG_STATUS      0x0FU
#define DS3231_REG_TEMP_MSB    0x11U

#define DS3231_STATUS_OSF      (1U << 7)

static inline uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

static inline uint8_t bin_to_bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10U) << 4) | (bin % 10U));
}

void DS3231_Init(DS3231_Handle_t *dev, I2C_HandleTypeDef *hi2c)
{
    dev->hi2c = hi2c;
}

HAL_StatusTypeDef DS3231_GetTime(DS3231_Handle_t *dev, DS3231_Time_t *out)
{
    uint8_t status_reg;
    uint8_t buf[7];
    HAL_StatusTypeDef status;

    status = HAL_I2C_Mem_Read(dev->hi2c, DS3231_ADDR, DS3231_REG_STATUS,
                               I2C_MEMADD_SIZE_8BIT, &status_reg, 1, DS3231_I2C_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        return status;
    }
    if ((status_reg & DS3231_STATUS_OSF) != 0U)
    {
        /* Oscillator stopped at some point -- time is not trustworthy. */
        return HAL_ERROR;
    }

    status = HAL_I2C_Mem_Read(dev->hi2c, DS3231_ADDR, DS3231_REG_SECONDS,
                               I2C_MEMADD_SIZE_8BIT, buf, sizeof(buf), DS3231_I2C_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        return status;
    }

    out->second = bcd_to_bin(buf[0] & 0x7FU);
    out->minute = bcd_to_bin(buf[1] & 0x7FU);
    out->hour   = bcd_to_bin(buf[2] & 0x3FU); /* assumes 24h mode (bit6=0) */
    out->date   = bcd_to_bin(buf[4] & 0x3FU);
    out->month  = bcd_to_bin(buf[5] & 0x1FU);
    out->year   = (uint16_t)(2000U + bcd_to_bin(buf[6]));

    return HAL_OK;
}

HAL_StatusTypeDef DS3231_SetTime(DS3231_Handle_t *dev, const DS3231_Time_t *time)
{
    uint8_t buf[7];
    uint8_t status_reg;
    HAL_StatusTypeDef status;

    buf[0] = bin_to_bcd(time->second);
    buf[1] = bin_to_bcd(time->minute);
    buf[2] = bin_to_bcd(time->hour); /* 24h mode: bit6=0, bit5=tens of hour */
    buf[3] = 1U; /* day-of-week: not tracked, keep at 1 */
    buf[4] = bin_to_bcd(time->date);
    buf[5] = bin_to_bcd(time->month);
    buf[6] = bin_to_bcd((uint8_t)(time->year - 2000U));

    status = HAL_I2C_Mem_Write(dev->hi2c, DS3231_ADDR, DS3231_REG_SECONDS,
                                I2C_MEMADD_SIZE_8BIT, buf, sizeof(buf), DS3231_I2C_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        return status;
    }

    /* Clear OSF now that a valid time has been written. */
    status = HAL_I2C_Mem_Read(dev->hi2c, DS3231_ADDR, DS3231_REG_STATUS,
                               I2C_MEMADD_SIZE_8BIT, &status_reg, 1, DS3231_I2C_TIMEOUT_MS);
    if (status != HAL_OK)
    {
        return status;
    }
    status_reg &= (uint8_t)~DS3231_STATUS_OSF;
    return HAL_I2C_Mem_Write(dev->hi2c, DS3231_ADDR, DS3231_REG_STATUS,
                              I2C_MEMADD_SIZE_8BIT, &status_reg, 1, DS3231_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef DS3231_GetTemperature(DS3231_Handle_t *dev, float *out_degC)
{
    uint8_t buf[2];
    HAL_StatusTypeDef status = HAL_I2C_Mem_Read(dev->hi2c, DS3231_ADDR, DS3231_REG_TEMP_MSB,
                                                 I2C_MEMADD_SIZE_8BIT, buf, sizeof(buf), DS3231_I2C_TIMEOUT_MS);
    if (status == HAL_OK)
    {
        int8_t msb = (int8_t)buf[0];
        uint8_t frac_quarters = (uint8_t)(buf[1] >> 6); /* top 2 bits = 0.25degC steps */
        *out_degC = (float)msb + (0.25f * (float)frac_quarters);
    }
    return status;
}
