/**
  * @file    usb_cdc_stdio.c
  * @brief   Retargets printf()/stdio to the USB CDC virtual COM port.
  *          Overrides the weak _write() from syscalls.c.
  */

#include "main.h"
#include "usbd_cdc_if.h"

int _write(int file, char *ptr, int len)
{
    (void)file;

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
