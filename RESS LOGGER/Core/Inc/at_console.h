/**
  * @file    at_console.h
  * @brief   Minimal line-based command console over the USB CDC link.
  *
  * Commands (case-insensitive, one per line, terminated by \r and/or \n):
  *   AT      -> "OK"
  *   CHECK   -> scans I2C1 bus + SD card, reports what's actually detected
  *   HELP    -> lists commands
  *
  * "check" only covers what has a driver today (I2C1 devices + SD/FatFs).
  * SPI devices (W5500 x2, W25Q16 flash) get added to the scan once their
  * drivers exist -- see ROADMAP notes in ress_sensors.h / project chat.
  */

#ifndef AT_CONSOLE_H
#define AT_CONSOLE_H

#include "main.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief Feed raw bytes received from the USB CDC OUT endpoint into the
  *        console's line buffer. Safe to call from ISR context (that's
  *        where CDC_Receive_FS lives) -- just copies into a ring buffer,
  *        no blocking calls.
  */
void AT_Console_OnUsbRx(const uint8_t *data, uint32_t len);

/**
  * @brief Call from the main loop. Drains any complete lines from the
  *        ring buffer and executes them. Safe to call every iteration;
  *        does nothing if no complete line is pending.
  */
void AT_Console_Process(void);

/**
  * @brief Whether the 1Hz sensor heartbeat print is currently enabled.
  *        Toggled by the "AT" command.
  */
bool AT_Console_HeartbeatEnabled(void);

#ifdef __cplusplus
}
#endif

#endif /* AT_CONSOLE_H */
