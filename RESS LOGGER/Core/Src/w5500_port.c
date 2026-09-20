#include "w5500_port.h"
#include "spi.h"
#include <string.h>

static W5500_Ctx_t s_ctx[W5500_COUNT];
static W5500_Ctx_t *s_current;

static void W5500_CS_Select(void)
{
    if (s_current == NULL) return;
    HAL_GPIO_WritePin(s_current->cs_port, s_current->cs_pin, GPIO_PIN_RESET);
}

static void W5500_CS_Deselect(void)
{
    if (s_current == NULL) return;
    HAL_GPIO_WritePin(s_current->cs_port, s_current->cs_pin, GPIO_PIN_SET);
}

static uint8_t W5500_SPI_ReadByte(void)
{
    uint8_t tx = 0xFF, rx = 0xFF;
    if (s_current == NULL) return 0xFF;
    HAL_SPI_TransmitReceive(s_current->hspi, &tx, &rx, 1, HAL_MAX_DELAY);
    return rx;
}

static void W5500_SPI_WriteByte(uint8_t wb)
{
    uint8_t rx;
    if (s_current == NULL) return;
    HAL_SPI_TransmitReceive(s_current->hspi, &wb, &rx, 1, HAL_MAX_DELAY);
}

static void W5500_SPI_ReadBurst(uint8_t *buf, uint16_t len)
{
    if (s_current == NULL) return;
    memset(buf, 0xFF, len);
    HAL_SPI_TransmitReceive(s_current->hspi, buf, buf, len, HAL_MAX_DELAY);
}

static void W5500_SPI_WriteBurst(uint8_t *buf, uint16_t len)
{
    uint8_t rx[1];
    if (s_current == NULL) return;
    /* HAL_SPI_TransmitReceive would need an equal-size rx buffer; reuse
     * buf in place is fine since W5500 doesn't care about MISO data during
     * a write-burst, so transmit-only avoids needing a second len-sized
     * scratch buffer. */
    (void)rx;
    HAL_SPI_Transmit(s_current->hspi, buf, len, HAL_MAX_DELAY);
}

void W5500_Port_Init(void)
{
    s_ctx[W5500_LAN].hspi     = &hspi1;
    s_ctx[W5500_LAN].cs_port  = W5500_1_CS_GPIO_Port;
    s_ctx[W5500_LAN].cs_pin   = W5500_1_CS_Pin;
    s_ctx[W5500_LAN].rst_port = W5500_1_RST_GPIO_Port;
    s_ctx[W5500_LAN].rst_pin  = W5500_1_RST_Pin;
    s_ctx[W5500_LAN].int_port = NULL;
    s_ctx[W5500_LAN].int_pin  = 0;

    s_ctx[W5500_WAN].hspi     = &hspi2;
    s_ctx[W5500_WAN].cs_port  = W5500_2_CS_GPIO_Port;
    s_ctx[W5500_WAN].cs_pin   = W5500_2_CS_Pin;
    s_ctx[W5500_WAN].rst_port = W5500_2_RST_GPIO_Port;
    s_ctx[W5500_WAN].rst_pin  = W5500_2_RST_Pin;
    s_ctx[W5500_WAN].int_port = W5500_2_INT_GPIO_Port;
    s_ctx[W5500_WAN].int_pin  = W5500_2_INT_Pin;

    /* Both CS lines idle high until a transfer selects one. */
    HAL_GPIO_WritePin(s_ctx[W5500_LAN].cs_port, s_ctx[W5500_LAN].cs_pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(s_ctx[W5500_WAN].cs_port, s_ctx[W5500_WAN].cs_pin, GPIO_PIN_SET);

    /* Registered once; the callbacks always act on whichever instance
     * W5500_Select() last pointed s_current at. */
    reg_wizchip_cs_cbfunc(W5500_CS_Select, W5500_CS_Deselect);
    reg_wizchip_spi_cbfunc(W5500_SPI_ReadByte, W5500_SPI_WriteByte);
    reg_wizchip_spiburst_cbfunc(W5500_SPI_ReadBurst, W5500_SPI_WriteBurst);
}

void W5500_Select(W5500_Instance_t inst)
{
    if (inst >= W5500_COUNT) return;
    s_current = &s_ctx[inst];
}

void W5500_Reset(W5500_Instance_t inst)
{
    if (inst >= W5500_COUNT) return;
    W5500_Ctx_t *ctx = &s_ctx[inst];

    /* W5500 datasheet: RSTn low pulse >= 500us, then wait >= 50ms before
     * accessing registers (PLL lock time). */
    HAL_GPIO_WritePin(ctx->rst_port, ctx->rst_pin, GPIO_PIN_RESET);
    HAL_Delay(1);
    HAL_GPIO_WritePin(ctx->rst_port, ctx->rst_pin, GPIO_PIN_SET);
    HAL_Delay(50);
}

uint8_t W5500_HardInit(W5500_Instance_t inst, const wiz_NetInfo *net)
{
    uint8_t txsize[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    uint8_t rxsize[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    wiz_NetInfo net_local;

    if (inst >= W5500_COUNT) return 0;

    W5500_Select(inst);
    W5500_Reset(inst);

    if (wizchip_init(txsize, rxsize) != 0) return 0;

    if (getVERSIONR() != 0x04) return 0;

    memcpy(&net_local, net, sizeof(net_local));
    wizchip_setnetinfo(&net_local);

    return 1;
}
