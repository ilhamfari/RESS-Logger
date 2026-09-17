#include "ads1115.h"

#define ADS1115_I2C_TIMEOUT_MS  50U

static HAL_StatusTypeDef ADS1115_WriteConfig(ADS1115_Handle_t *dev, uint16_t config)
{
    uint8_t buf[2] = { (uint8_t)(config >> 8), (uint8_t)(config & 0xFF) };
    return HAL_I2C_Mem_Write(dev->hi2c, dev->i2c_addr, ADS1115_REG_CONFIG,
                              I2C_MEMADD_SIZE_8BIT, buf, sizeof(buf), ADS1115_I2C_TIMEOUT_MS);
}

static HAL_StatusTypeDef ADS1115_WriteReg16(ADS1115_Handle_t *dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[2] = { (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return HAL_I2C_Mem_Write(dev->hi2c, dev->i2c_addr, reg,
                              I2C_MEMADD_SIZE_8BIT, buf, sizeof(buf), ADS1115_I2C_TIMEOUT_MS);
}

void ADS1115_Init(ADS1115_Handle_t *dev, I2C_HandleTypeDef *hi2c, uint16_t i2c_addr, uint16_t pga_config)
{
    dev->hi2c = hi2c;
    dev->i2c_addr = i2c_addr;
    dev->pga_config = pga_config;
}

HAL_StatusTypeDef ADS1115_ConfigConversionReadyPin(ADS1115_Handle_t *dev)
{
    HAL_StatusTypeDef status;

    /* Hi_thresh MSB = 1, Lo_thresh MSB = 0 selects conversion-ready mode
     * for the ALERT/RDY pin (ADS1115 datasheet). */
    status = ADS1115_WriteReg16(dev, ADS1115_REG_HI_THRESH, 0x8000U);
    if (status != HAL_OK)
    {
        return status;
    }
    return ADS1115_WriteReg16(dev, ADS1115_REG_LO_THRESH, 0x0000U);
}

HAL_StatusTypeDef ADS1115_StartSingleConversion(ADS1115_Handle_t *dev, ADS1115_Channel_t channel)
{
    static const uint16_t mux_for_channel[4] = {
        ADS1115_MUX_AIN0_GND,
        ADS1115_MUX_AIN1_GND,
        ADS1115_MUX_AIN2_GND,
        ADS1115_MUX_AIN3_GND,
    };

    uint16_t config = ADS1115_OS_SINGLE_START
                     | mux_for_channel[channel]
                     | dev->pga_config
                     | ADS1115_MODE_SINGLE_SHOT
                     | ADS1115_DR_128SPS
                     | ADS1115_COMP_MODE_TRAD
                     | ADS1115_COMP_POL_ACTIVE_LOW
                     | ADS1115_COMP_LAT_DISABLE
                     | ADS1115_COMP_QUE_ONE;

    return ADS1115_WriteConfig(dev, config);
}

HAL_StatusTypeDef ADS1115_ReadRaw(ADS1115_Handle_t *dev, int16_t *out_raw)
{
    uint8_t buf[2];
    HAL_StatusTypeDef status = HAL_I2C_Mem_Read(dev->hi2c, dev->i2c_addr, ADS1115_REG_CONVERSION,
                                                 I2C_MEMADD_SIZE_8BIT, buf, sizeof(buf), ADS1115_I2C_TIMEOUT_MS);
    if (status == HAL_OK)
    {
        *out_raw = (int16_t)((buf[0] << 8) | buf[1]);
    }
    return status;
}

int32_t ADS1115_RawToMillivolts(const ADS1115_Handle_t *dev, int16_t raw)
{
    int32_t fs_mv;

    switch (dev->pga_config)
    {
        case ADS1115_PGA_6144MV: fs_mv = 6144; break;
        case ADS1115_PGA_4096MV: fs_mv = 4096; break;
        case ADS1115_PGA_2048MV: fs_mv = 2048; break;
        case ADS1115_PGA_1024MV: fs_mv = 1024; break;
        case ADS1115_PGA_0512MV: fs_mv = 512;  break;
        case ADS1115_PGA_0256MV: fs_mv = 256;  break;
        default:                 fs_mv = 2048; break;
    }

    /* raw is signed 16-bit, full scale = +-fs_mv over +-32768 */
    return ((int32_t)raw * fs_mv) / 32768;
}
