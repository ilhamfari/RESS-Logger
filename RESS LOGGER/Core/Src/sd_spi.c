/**
  * @file    sd_spi.c
  * @brief   SD/SDHC/SDXC block driver over SPI2. See sd_spi.h.
  *
  *          Protocol notes:
  *          - Identification phase (CMD0..CMD58) must run at <=400kHz per
  *            the SD spec, so SPI2 is dropped to its slowest prescaler for
  *            SD_SPI_Init() and only raised back to the normal (fast)
  *            prescaler once the card reports data-transfer-ready.
  *          - Data-line CRC (CRC16 on 512-byte blocks) is left disabled
  *            (the card defaults to CRC-off in SPI mode; CMD59 is never
  *            sent to turn it on), so the two CRC bytes after each data
  *            block are read and discarded rather than verified.
  *          - Multi-block read/write (CMD18/CMD25) is used whenever FatFs
  *            asks for more than one sector at a time; ACMD23 pre-erases
  *            the target range before a multi-block write to avoid the
  *            card doing it one block at a time.
  */

#include "sd_spi.h"
#include "spi.h"
#include "iwdg.h"
#include <string.h>

#define CMD0    (0U)         /* GO_IDLE_STATE */
#define CMD1    (1U)         /* SEND_OP_COND (MMC) */
#define CMD8    (8U)         /* SEND_IF_COND */
#define CMD9    (9U)         /* SEND_CSD */
#define CMD12   (12U)        /* STOP_TRANSMISSION */
#define CMD16   (16U)        /* SET_BLOCKLEN */
#define CMD17   (17U)        /* READ_SINGLE_BLOCK */
#define CMD18   (18U)        /* READ_MULTIPLE_BLOCK */
#define CMD23   (23U)        /* SET_WR_BLOCK_ERASE_COUNT (ACMD) */
#define CMD24   (24U)        /* WRITE_BLOCK */
#define CMD25   (25U)        /* WRITE_MULTIPLE_BLOCK */
#define CMD41   (41U)        /* SEND_OP_COND (ACMD) */
#define CMD55   (55U)        /* APP_CMD */
#define CMD58   (58U)        /* READ_OCR */
#define ACMD(n) (0x80U | (n))

#define CT_MMC    0x01U
#define CT_SD1    0x02U
#define CT_SD2    0x04U
#define CT_BLOCK  0x08U

#define SD_INIT_TIMEOUT_MS  1000U
#define SD_BUSY_TIMEOUT_MS  500U
#define SD_READ_TIMEOUT_MS  200U

static volatile uint8_t s_initialized;
static uint8_t  s_card_type;
static uint32_t s_sector_count;
static uint8_t  s_ff[SD_SPI_BLOCK_SIZE]; /* all-0xFF scratch TX buffer for clocking in read data */

static void CS_Low(void)
{
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET);
}

static void CS_High(void)
{
    HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);
}

static uint8_t SPI_TxRx(uint8_t d)
{
    uint8_t rx = 0xFFU;
    HAL_SPI_TransmitReceive(&hspi2, &d, &rx, 1, HAL_MAX_DELAY);
    return rx;
}

/* Must only be called while SPI2 is idle (not mid-transaction). */
static void SPI_SetBaud(uint32_t prescaler)
{
    __HAL_SPI_DISABLE(&hspi2);
    hspi2.Instance->CR1 = (hspi2.Instance->CR1 & ~SPI_CR1_BR) | prescaler;
    __HAL_SPI_ENABLE(&hspi2);
}

static uint8_t SD_WaitReady(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t r;

    do
    {
        r = SPI_TxRx(0xFFU);
        if ((HAL_GetTick() - start) > 200U)
        {
            HAL_IWDG_Refresh(&hiwdg); /* a slow card's busy wait can approach the IWDG period on its own */
        }
    } while (r != 0xFFU && (HAL_GetTick() - start) < timeout_ms);

    return (r == 0xFFU);
}

static uint8_t SD_SendCmd(uint8_t cmd, uint32_t arg)
{
    uint8_t crc, r1, n;

    if (cmd & 0x80U)
    {
        /* ACMD<n> is CMD55 followed by CMD<n> */
        cmd &= 0x7FU;
        r1 = SD_SendCmd(CMD55, 0);
        if (r1 > 1U)
        {
            return r1;
        }
    }

    if (cmd != CMD0 && cmd != CMD12)
    {
        CS_High();
        SPI_TxRx(0xFFU);
        CS_Low();
        if (!SD_WaitReady(SD_BUSY_TIMEOUT_MS))
        {
            return 0xFFU;
        }
    }
    else
    {
        CS_Low();
    }

    SPI_TxRx((uint8_t)(0x40U | cmd));
    SPI_TxRx((uint8_t)(arg >> 24));
    SPI_TxRx((uint8_t)(arg >> 16));
    SPI_TxRx((uint8_t)(arg >> 8));
    SPI_TxRx((uint8_t)arg);

    crc = 0x01U;
    if (cmd == CMD0) crc = 0x95U;
    if (cmd == CMD8) crc = 0x87U;
    SPI_TxRx(crc);

    if (cmd == CMD12)
    {
        SPI_TxRx(0xFFU); /* stuff byte for STOP_TRANSMISSION */
    }

    n = 10U;
    do
    {
        r1 = SPI_TxRx(0xFFU);
    } while ((r1 & 0x80U) && --n);

    return r1;
}

static uint8_t SD_ReceiveDataBlock(uint8_t *buff, uint32_t len)
{
    uint8_t token;
    uint32_t start = HAL_GetTick();

    do
    {
        token = SPI_TxRx(0xFFU);
    } while (token == 0xFFU && (HAL_GetTick() - start) < SD_READ_TIMEOUT_MS);

    if (token != 0xFEU)
    {
        return 0U;
    }

    HAL_SPI_TransmitReceive(&hspi2, s_ff, buff, (uint16_t)len, HAL_MAX_DELAY);

    /* CRC16 trailer -- read and discard (data-line CRC is left disabled). */
    SPI_TxRx(0xFFU);
    SPI_TxRx(0xFFU);

    return 1U;
}

static uint8_t SD_SendDataBlock(const uint8_t *buff, uint8_t token)
{
    uint8_t resp;

    if (!SD_WaitReady(SD_BUSY_TIMEOUT_MS))
    {
        return 0U;
    }

    SPI_TxRx(token);

    if (token != 0xFDU) /* 0xFD = stop-tran token, carries no data */
    {
        HAL_SPI_Transmit(&hspi2, (uint8_t *)buff, SD_SPI_BLOCK_SIZE, HAL_MAX_DELAY);

        SPI_TxRx(0xFFU); /* dummy CRC16 */
        SPI_TxRx(0xFFU);

        resp = SPI_TxRx(0xFFU);
        if ((resp & 0x1FU) != 0x05U) /* data response token: xxx00101 = accepted */
        {
            return 0U;
        }
    }

    return 1U;
}

static uint8_t SD_ReadCSDSectorCount(uint32_t *out_sectors)
{
    uint8_t csd[16];
    uint32_t sectors;

    if (SD_SendCmd(CMD9, 0) != 0U)
    {
        return 0U;
    }
    if (!SD_ReceiveDataBlock(csd, sizeof(csd)))
    {
        return 0U;
    }

    if ((csd[0] >> 6) == 1U) /* CSD version 2.0 -- SDHC/SDXC */
    {
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3FU) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        sectors = (c_size + 1UL) * 1024UL; /* capacity = (C_SIZE+1) * 512KB */
    }
    else /* CSD version 1.0 -- SDSC */
    {
        uint32_t c_size      = (((uint32_t)csd[6] & 0x03U) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        uint32_t c_size_mult = (uint32_t)((csd[9] & 0x03U) << 1) | (csd[10] >> 7);
        uint32_t read_bl_len = csd[5] & 0x0FU;
        uint32_t block_len   = 1UL << read_bl_len;
        uint32_t mult        = 1UL << (c_size_mult + 2U);
        uint32_t capacity_bytes = (c_size + 1UL) * mult * block_len;
        sectors = capacity_bytes / SD_SPI_BLOCK_SIZE;
    }

    *out_sectors = sectors;
    return (sectors != 0U);
}

uint8_t SD_SPI_Init(void)
{
    uint8_t n, cmd, ty;
    uint8_t resp[4];
    uint32_t tmr;

    s_initialized = 0U;
    s_card_type = 0U;
    s_sector_count = 0U;
    memset(s_ff, 0xFFU, sizeof(s_ff));

    SPI_SetBaud(SPI_BAUDRATEPRESCALER_256); /* <=400kHz for the identification phase */
    CS_High();
    for (n = 0U; n < 10U; n++)
    {
        SPI_TxRx(0xFFU); /* >=74 clocks with CS high, MOSI high */
    }

    ty = 0U;
    if (SD_SendCmd(CMD0, 0) == 1U) /* enter idle state */
    {
        tmr = HAL_GetTick();
        if (SD_SendCmd(CMD8, 0x1AAUL) == 1U) /* SDv2? */
        {
            for (n = 0U; n < 4U; n++)
            {
                resp[n] = SPI_TxRx(0xFFU); /* R7 trailer: echoed voltage/check pattern */
            }
            if (resp[2] == 0x01U && resp[3] == 0xAAU) /* card supports 2.7-3.6V */
            {
                while (((HAL_GetTick() - tmr) < SD_INIT_TIMEOUT_MS) &&
                       SD_SendCmd(ACMD(CMD41), 1UL << 30)) /* ACMD41 with HCS */
                {
                    HAL_IWDG_Refresh(&hiwdg); /* this loop alone can run longer than the IWDG period */
                }
                if (((HAL_GetTick() - tmr) < SD_INIT_TIMEOUT_MS) && SD_SendCmd(CMD58, 0) == 0U)
                {
                    for (n = 0U; n < 4U; n++)
                    {
                        resp[n] = SPI_TxRx(0xFFU);
                    }
                    ty = (resp[0] & 0x40U) ? (CT_SD2 | CT_BLOCK) : CT_SD2;
                }
            }
        }
        else /* SDv1 or MMCv3 */
        {
            if (SD_SendCmd(ACMD(CMD41), 0) <= 1U)
            {
                ty = CT_SD1;
                cmd = ACMD(CMD41);
            }
            else
            {
                ty = CT_MMC;
                cmd = CMD1;
            }
            while (((HAL_GetTick() - tmr) < SD_INIT_TIMEOUT_MS) && SD_SendCmd(cmd, 0))
            {
                HAL_IWDG_Refresh(&hiwdg); /* this loop alone can run longer than the IWDG period */
            }
            if (!((HAL_GetTick() - tmr) < SD_INIT_TIMEOUT_MS) || SD_SendCmd(CMD16, SD_SPI_BLOCK_SIZE) != 0U)
            {
                ty = 0U;
            }
        }
    }

    s_card_type = ty;
    CS_High();
    SPI_TxRx(0xFFU);

    if (ty != 0U && SD_ReadCSDSectorCount(&s_sector_count))
    {
        SPI_SetBaud(SPI_BAUDRATEPRESCALER_2); /* back to normal speed for data transfer */
        s_initialized = 1U;
    }
    else
    {
        CS_High();
    }

    return s_initialized;
}

uint8_t SD_SPI_IsInitialized(void)
{
    return s_initialized;
}

uint32_t SD_SPI_GetSectorCount(void)
{
    return s_sector_count;
}

uint8_t SD_SPI_ReadBlocks(uint8_t *buff, uint32_t sector, uint32_t count)
{
    if (!s_initialized || count == 0U)
    {
        return 0U;
    }

    if (!(s_card_type & CT_BLOCK))
    {
        sector *= SD_SPI_BLOCK_SIZE; /* byte addressing on SDSC/MMC */
    }

    if (count == 1U)
    {
        if ((SD_SendCmd(CMD17, sector) == 0U) && SD_ReceiveDataBlock(buff, SD_SPI_BLOCK_SIZE))
        {
            count = 0U;
        }
    }
    else
    {
        if (SD_SendCmd(CMD18, sector) == 0U)
        {
            do
            {
                HAL_IWDG_Refresh(&hiwdg); /* a long multi-block transfer can outrun the IWDG period */
                if (!SD_ReceiveDataBlock(buff, SD_SPI_BLOCK_SIZE))
                {
                    break;
                }
                buff += SD_SPI_BLOCK_SIZE;
            } while (--count);
            SD_SendCmd(CMD12, 0); /* stop transmission */
        }
    }

    CS_High();
    SPI_TxRx(0xFFU);

    return (count == 0U);
}

uint8_t SD_SPI_WriteBlocks(const uint8_t *buff, uint32_t sector, uint32_t count)
{
    if (!s_initialized || count == 0U)
    {
        return 0U;
    }

    if (!(s_card_type & CT_BLOCK))
    {
        sector *= SD_SPI_BLOCK_SIZE; /* byte addressing on SDSC/MMC */
    }

    if (count == 1U)
    {
        if ((SD_SendCmd(CMD24, sector) == 0U) && SD_SendDataBlock(buff, 0xFEU))
        {
            count = 0U;
        }
    }
    else
    {
        if (s_card_type & (CT_SD1 | CT_SD2))
        {
            SD_SendCmd(ACMD(CMD23), count); /* pre-erase the target range */
        }
        if (SD_SendCmd(CMD25, sector) == 0U)
        {
            do
            {
                HAL_IWDG_Refresh(&hiwdg); /* a long multi-block transfer can outrun the IWDG period */
                if (!SD_SendDataBlock(buff, 0xFCU))
                {
                    break;
                }
                buff += SD_SPI_BLOCK_SIZE;
            } while (--count);

            if (!SD_SendDataBlock(NULL, 0xFDU)) /* stop-tran token */
            {
                count = 1U;
            }
        }
    }

    CS_High();
    SPI_TxRx(0xFFU);

    return (count == 0U);
}
