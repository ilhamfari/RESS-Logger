#ifndef W5500_PORT_H
#define W5500_PORT_H

#include "main.h"
#include "spi.h"
#include "wizchip_conf.h"

typedef enum
{
    W5500_LAN = 0,
    W5500_WAN = 1,
    W5500_COUNT
} W5500_Instance_t;

typedef struct
{
    SPI_HandleTypeDef *hspi;
    GPIO_TypeDef *cs_port;  uint16_t cs_pin;
    GPIO_TypeDef *rst_port; uint16_t rst_pin;
    GPIO_TypeDef *int_port; uint16_t int_pin;   /* int_port == NULL: unused (LAN in v1) */
} W5500_Ctx_t;

/* Registers HAL SPI/GPIO callbacks with ioLibrary (once) and prepares the
 * per-instance context table. Must run after MX_SPI1_Init()/MX_SPI3_Init()
 * and MX_GPIO_Init(). */
void W5500_Port_Init(void);

/* Points every subsequent ioLibrary call at this chip's SPI/CS/RST. ioLibrary
 * itself has no concept of multiple chips, so every call site touching a
 * given instance's registers/sockets must call this first. */
void W5500_Select(W5500_Instance_t inst);

/* Hardware reset pulse per W5500 datasheet timing (active-low RST). */
void W5500_Reset(W5500_Instance_t inst);

/* Full bring-up: select -> reset -> wizchip_init -> setnetinfo -> verify
 * chip version register. Returns 1 on success (VERSIONR == 0x04), 0 on
 * failure (chip not responding / mis-wired). */
uint8_t W5500_HardInit(W5500_Instance_t inst, const wiz_NetInfo *net);

#endif /* W5500_PORT_H */
