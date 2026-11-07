/**
  * @file    ress_util.h
  * @brief   Small shared helpers: debug logging macro (printf over USB CDC
  *          serial monitor) and a float formatter that does not depend on
  *          newlib-nano's printf float support.
  */

#ifndef RESS_UTIL_H
#define RESS_UTIL_H

#include "main.h"
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * Debug line on the serial monitor (USB CDC), e.g.:
  *   [   12345] MODBUS  poll ok
  * Never pass float arguments to RESS_LOG (nano printf has no %f); use
  * RESS_FmtFixed() instead.
  */
#define RESS_LOG(tag, fmt, ...) \
    printf("[%8lu] %-7s " fmt "\r\n", (unsigned long)HAL_GetTick(), (tag), ##__VA_ARGS__)

/**
  * @brief Format a float as fixed-point text with `decimals` digits
  *        (0..4), rounded. Writes a NUL-terminated string into buf.
  * @retval Number of characters written (excluding NUL), like snprintf.
  */
int RESS_FmtFixed(char *buf, size_t size, float value, uint8_t decimals);

#ifdef __cplusplus
}
#endif

#endif /* RESS_UTIL_H */
