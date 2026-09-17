/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file   fatfs.c
  * @brief  Code for fatfs applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
#include "fatfs.h"
#include "ds3231.h"
#include "i2c.h"

uint8_t retSD;    /* Return value for SD */
char SDPath[4];   /* SD logical drive path */
FATFS SDFatFS;    /* File system object for SD logical drive */
FIL SDFile;       /* File object for SD */

/* USER CODE BEGIN Variables */

/* USER CODE END Variables */

void MX_FATFS_Init(void)
{
  /*## FatFS: Link the SD driver ###########################*/
  retSD = FATFS_LinkDriver(&SD_Driver, SDPath);

  /* USER CODE BEGIN Init */
  /* additional user code for init */
  /* USER CODE END Init */
}

/**
  * @brief  Gets Time from RTC
  * @param  None
  * @retval Time in DWORD
  */
DWORD get_fattime(void)
{
  /* USER CODE BEGIN get_fattime */
  static DS3231_Handle_t fattime_rtc;
  static bool fattime_rtc_ready = false;
  DS3231_Time_t now;

  if (!fattime_rtc_ready)
  {
    DS3231_Init(&fattime_rtc, &hi2c1);
    fattime_rtc_ready = true;
  }

  if (DS3231_GetTime(&fattime_rtc, &now) != HAL_OK)
  {
    /* RTC unreadable/untrustworthy -- fall back to FatFs epoch (1980-01-01)
     * rather than fabricating a time that looks valid but isn't. */
    return 0;
  }

  return ((DWORD)(now.year - 1980) << 25)
       | ((DWORD)now.month << 21)
       | ((DWORD)now.date << 16)
       | ((DWORD)now.hour << 11)
       | ((DWORD)now.minute << 5)
       | ((DWORD)(now.second / 2));
  /* USER CODE END get_fattime */
}

/* USER CODE BEGIN Application */

/* USER CODE END Application */
