#include "ress_sensors.h"

/* Divider ratio for the 0-10V channels: Vout = Vin * R5/(R2+R5).
 * R2=175k, R5=75k -> ratio = 75/250 = 0.3, so 10V field -> ~3.0V at pin. */
#define RESS_VOLTAGE_DIVIDER_RATIO   0.3f

/* 4-20mA sense shunt resistance (R14/R18), ohms. mA = mV_at_pin / R_ohm. */
#define RESS_CURRENT_SHUNT_OHMS      150.0f

static ADS1115_Handle_t s_ads1115;
static volatile RESS_Channel_t s_next_channel;
static RESS_SensorData_t s_latest;
static volatile uint8_t s_channels_seen; /* bitmask, set once all 4 read at least once */

static void RESS_Sensors_StoreReading(RESS_Channel_t channel, int16_t raw)
{
    int32_t mv_at_pin = ADS1115_RawToMillivolts(&s_ads1115, raw);

    switch (channel)
    {
        case RESS_CH_VOLTAGE_0:
            s_latest.voltage_0_V = (mv_at_pin / 1000.0f) / RESS_VOLTAGE_DIVIDER_RATIO;
            break;
        case RESS_CH_VOLTAGE_1:
            s_latest.voltage_1_V = (mv_at_pin / 1000.0f) / RESS_VOLTAGE_DIVIDER_RATIO;
            break;
        case RESS_CH_CURRENT_0:
            s_latest.current_0_mA = mv_at_pin / RESS_CURRENT_SHUNT_OHMS;
            break;
        case RESS_CH_CURRENT_1:
            s_latest.current_1_mA = mv_at_pin / RESS_CURRENT_SHUNT_OHMS;
            break;
        default:
            break;
    }

    s_channels_seen |= (1U << channel);
    if (s_channels_seen == 0x0FU)
    {
        s_latest.valid = true;
    }
}

void RESS_Sensors_Init(I2C_HandleTypeDef *hi2c)
{
    ADS1115_Init(&s_ads1115, hi2c, ADS1115_ADDR, ADS1115_PGA_4096MV);

    s_latest.valid = false;
    s_channels_seen = 0;
    s_next_channel = RESS_CH_VOLTAGE_0;

    ADS1115_StartSingleConversion(&s_ads1115, (ADS1115_Channel_t)s_next_channel);
}

void RESS_Sensors_OnConversionReady(void)
{
    int16_t raw;
    RESS_Channel_t completed_channel = s_next_channel;

    if (ADS1115_ReadRaw(&s_ads1115, &raw) == HAL_OK)
    {
        RESS_Sensors_StoreReading(completed_channel, raw);
    }

    s_next_channel = (RESS_Channel_t)((completed_channel + 1U) % RESS_CH_COUNT);
    ADS1115_StartSingleConversion(&s_ads1115, (ADS1115_Channel_t)s_next_channel);
}

void RESS_Sensors_GetLatest(RESS_SensorData_t *out)
{
    __disable_irq();
    *out = s_latest;
    __enable_irq();
}
