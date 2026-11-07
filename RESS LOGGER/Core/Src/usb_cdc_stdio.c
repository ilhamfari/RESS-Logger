/**
  * @file    usb_cdc_stdio.c
  * @brief   Retargets printf()/stdio to the USB CDC virtual COM port.
  *          Overrides the weak _write() from syscalls.c.
  */

#include "main.h"
#include "usbd_cdc_if.h"
extern USBD_HandleTypeDef hUsbDeviceFS;

int _write(int file, char *ptr, int len)
{
    (void)file;

    /* No host has configured the CDC port: drop instead of stalling the
     * main loop 100 ms on every debug line. */
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
    {
        return len;
    }

    /* USB not enumerated / no terminal open yet -- drop the data instead
     * of blocking forever so the app doesn't hang without a host attached. */
    uint32_t timeout = HAL_GetTick() + 100U;

    while (CDC_Transmit_FS((uint8_t *)ptr, (uint16_t)len) == USBD_BUSY)
    {
        if (HAL_GetTick() > timeout)
        {
            return 0;
        }
    }

    return len;
}
