#include "w25q16.h"

#define W25Q16_SPI_TIMEOUT_MS      100U
#define W25Q16_WRITE_CYCLE_MS      5U    /* page program, datasheet tPP max ~3ms */
#define W25Q16_ERASE_TIMEOUT_MS    500U  /* sector erase, datasheet tSE max ~400ms */
#define W25Q16_BUSY_POLL_MS        1U

#define W25Q16_CMD_WRITE_ENABLE    0x06U
#define W25Q16_CMD_READ_STATUS1    0x05U
#define W25Q16_CMD_PAGE_PROGRAM    0x02U
#define W25Q16_CMD_READ_DATA       0x03U
#define W25Q16_CMD_SECTOR_ERASE    0x20U
#define W25Q16_CMD_JEDEC_ID        0x9FU

#define W25Q16_STATUS1_BUSY        0x01U

static void W25Q16_CS_Low(W25Q16_Handle_t *dev)
{
    HAL_GPIO_WritePin(dev->cs_port, dev->cs_pin, GPIO_PIN_RESET);
}

static void W25Q16_CS_High(W25Q16_Handle_t *dev)
{
    HAL_GPIO_WritePin(dev->cs_port, dev->cs_pin, GPIO_PIN_SET);
}

static HAL_StatusTypeDef W25Q16_WriteEnable(W25Q16_Handle_t *dev)
{
    uint8_t cmd = W25Q16_CMD_WRITE_ENABLE;
    W25Q16_CS_Low(dev);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(dev->hspi, &cmd, 1, W25Q16_SPI_TIMEOUT_MS);
    W25Q16_CS_High(dev);
    return status;
}

static HAL_StatusTypeDef W25Q16_ReadStatus1(W25Q16_Handle_t *dev, uint8_t *status_out)
{
    uint8_t cmd = W25Q16_CMD_READ_STATUS1;
    W25Q16_CS_Low(dev);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(dev->hspi, &cmd, 1, W25Q16_SPI_TIMEOUT_MS);
    if (status == HAL_OK)
    {
        status = HAL_SPI_Receive(dev->hspi, status_out, 1, W25Q16_SPI_TIMEOUT_MS);
    }
    W25Q16_CS_High(dev);
    return status;
}

/* Polls status register 1 until BUSY clears or timeout_ms elapses. */
static HAL_StatusTypeDef W25Q16_WaitReady(W25Q16_Handle_t *dev, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t sr1;

    do
    {
        HAL_StatusTypeDef status = W25Q16_ReadStatus1(dev, &sr1);
        if (status != HAL_OK)
        {
            return status;
        }
        if ((sr1 & W25Q16_STATUS1_BUSY) == 0U)
        {
            return HAL_OK;
        }
        HAL_Delay(W25Q16_BUSY_POLL_MS);
    } while ((HAL_GetTick() - start) < timeout_ms);

    return HAL_TIMEOUT;
}

/* Builds a 3-byte big-endian address prefix as used by all W25Q commands. */
static void W25Q16_AddrToBytes(uint32_t addr, uint8_t out[3])
{
    out[0] = (uint8_t)(addr >> 16);
    out[1] = (uint8_t)(addr >> 8);
    out[2] = (uint8_t)(addr);
}

void W25Q16_Init(W25Q16_Handle_t *dev, SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
    dev->hspi = hspi;
    dev->cs_port = cs_port;
    dev->cs_pin = cs_pin;
    W25Q16_CS_High(dev);
}

HAL_StatusTypeDef W25Q16_ReadJedecId(W25Q16_Handle_t *dev, uint8_t id[3])
{
    uint8_t cmd = W25Q16_CMD_JEDEC_ID;

    W25Q16_CS_Low(dev);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(dev->hspi, &cmd, 1, W25Q16_SPI_TIMEOUT_MS);
    if (status == HAL_OK)
    {
        status = HAL_SPI_Receive(dev->hspi, id, 3, W25Q16_SPI_TIMEOUT_MS);
    }
    W25Q16_CS_High(dev);
    return status;
}

HAL_StatusTypeDef W25Q16_Read(W25Q16_Handle_t *dev, uint32_t addr, uint8_t *data, uint32_t len)
{
    uint8_t header[4];
    header[0] = W25Q16_CMD_READ_DATA;
    W25Q16_AddrToBytes(addr, &header[1]);

    W25Q16_CS_Low(dev);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(dev->hspi, header, sizeof(header), W25Q16_SPI_TIMEOUT_MS);
    if (status == HAL_OK)
    {
        status = HAL_SPI_Receive(dev->hspi, data, len, W25Q16_SPI_TIMEOUT_MS);
    }
    W25Q16_CS_High(dev);
    return status;
}

HAL_StatusTypeDef W25Q16_Write(W25Q16_Handle_t *dev, uint32_t addr, const uint8_t *data, uint32_t len)
{
    while (len > 0U)
    {
        uint32_t offset_in_page = addr % W25Q16_PAGE_SIZE;
        uint32_t space_in_page = W25Q16_PAGE_SIZE - offset_in_page;
        uint32_t chunk = (len < space_in_page) ? len : space_in_page;

        HAL_StatusTypeDef status = W25Q16_WriteEnable(dev);
        if (status != HAL_OK)
        {
            return status;
        }

        uint8_t header[4];
        header[0] = W25Q16_CMD_PAGE_PROGRAM;
        W25Q16_AddrToBytes(addr, &header[1]);

        W25Q16_CS_Low(dev);
        status = HAL_SPI_Transmit(dev->hspi, header, sizeof(header), W25Q16_SPI_TIMEOUT_MS);
        if (status == HAL_OK)
        {
            status = HAL_SPI_Transmit(dev->hspi, (uint8_t *)data, chunk, W25Q16_SPI_TIMEOUT_MS);
        }
        W25Q16_CS_High(dev);
        if (status != HAL_OK)
        {
            return status;
        }

        status = W25Q16_WaitReady(dev, W25Q16_WRITE_CYCLE_MS * 10U);
        if (status != HAL_OK)
        {
            return status;
        }

        addr += chunk;
        data += chunk;
        len -= chunk;
    }

    return HAL_OK;
}

HAL_StatusTypeDef W25Q16_EraseSector(W25Q16_Handle_t *dev, uint32_t addr)
{
    uint32_t sector_addr = addr - (addr % W25Q16_SECTOR_SIZE);

    HAL_StatusTypeDef status = W25Q16_WriteEnable(dev);
    if (status != HAL_OK)
    {
        return status;
    }

    uint8_t header[4];
    header[0] = W25Q16_CMD_SECTOR_ERASE;
    W25Q16_AddrToBytes(sector_addr, &header[1]);

    W25Q16_CS_Low(dev);
    status = HAL_SPI_Transmit(dev->hspi, header, sizeof(header), W25Q16_SPI_TIMEOUT_MS);
    W25Q16_CS_High(dev);
    if (status != HAL_OK)
    {
        return status;
    }

    return W25Q16_WaitReady(dev, W25Q16_ERASE_TIMEOUT_MS);
}
