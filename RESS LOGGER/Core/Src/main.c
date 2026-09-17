/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"
#include "i2c.h"
#include "iwdg.h"
#include "sdio.h"
#include "spi.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ress_sensors.h"
#include "ds3231.h"
#include "at24c256.h"
#include "at_console.h"
#include <stdio.h>

extern volatile uint32_t g_fault_marker; /* set by stm32f4xx_it.c fault handlers */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static DS3231_Handle_t   rtc;
static AT24C256_Handle_t eeprom;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_SDIO_SD_Init();
  MX_SPI1_Init();
  MX_SPI3_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_USART3_UART_Init();
  MX_IWDG_Init();
  MX_FATFS_Init();
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */
  /* CubeMX's generated MX_GPIO_Init() drives every output pin's initial
   * level to RESET before configuring it, which leaves these active-low
   * SPI chip-select lines asserted (selected) at boot -- gpio.c has no
   * per-pin USER CODE hook to override this without CubeMX wiping it on
   * regen, so fix it here instead. FLASH_CS (PA15) and W5500_2_CS (PD4)
   * share the SPI3 bus, so both must idle high or the W25Q16 flash check
   * would contend with the W5500 #2 chip for MISO. */
  HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(W5500_2_CS_GPIO_Port, W5500_2_CS_Pin, GPIO_PIN_SET);

  /* Capture and clear reset cause + fault marker now, before anything else
   * can touch them; the diagnostic print happens later once USB CDC is up. */
  uint32_t reset_flags = RCC->CSR;
  uint32_t fault_marker = g_fault_marker;
  __HAL_RCC_CLEAR_RESET_FLAGS();
  g_fault_marker = 0;

  HAL_IWDG_Refresh(&hiwdg);

  RESS_Sensors_Init(&hi2c1);

  HAL_IWDG_Refresh(&hiwdg);

  DS3231_Init(&rtc, &hi2c1);
  {
    DS3231_Time_t now;
    if (DS3231_GetTime(&rtc, &now) != HAL_OK)
    {
      /* OSF set (lost power) or I2C error -- RTC time is not trustworthy.
       * TODO: replace this placeholder with real provisioning (e.g. set
       * from a build-time constant, a config command over UART/network,
       * or NTP once a network link is up) instead of a fixed fallback. */
      DS3231_Time_t fallback = { .year = 2026, .month = 1, .date = 1,
                                  .hour = 0, .minute = 0, .second = 0 };
      DS3231_SetTime(&rtc, &fallback);
    }
  }

  AT24C256_Init(&eeprom, &hi2c1);

  /* USB CDC needs the host to enumerate the device before it can accept
   * data; give it a moment on cold boot so this first message isn't lost.
   * IWDG timeout is ~512ms, so this wait is chunked with refreshes
   * instead of one blocking HAL_Delay -- a single long delay here would
   * let the watchdog reset the MCU mid-enumeration, forever. */
  for (uint32_t i = 0; i < 15; i++)
  {
    HAL_IWDG_Refresh(&hiwdg);
    HAL_Delay(100);
  }
  printf("RESS Logger boot OK\r\n");

  if (reset_flags & RCC_CSR_IWDGRSTF)  printf("Reset cause: IWDG (watchdog)\r\n");
  if (reset_flags & RCC_CSR_PORRSTF)   printf("Reset cause: POR/PDR (power-on/down)\r\n");
  if (reset_flags & RCC_CSR_PINRSTF)   printf("Reset cause: NRST pin\r\n");
  if (reset_flags & RCC_CSR_SFTRSTF)   printf("Reset cause: software (NVIC_SystemReset)\r\n");
  if (reset_flags & RCC_CSR_LPWRRSTF)  printf("Reset cause: low-power\r\n");
  if (reset_flags & RCC_CSR_WWDGRSTF)  printf("Reset cause: WWDG (window watchdog)\r\n");
  if (fault_marker != 0)
  {
    printf("Fault before reset: marker=0x%08lX\r\n", (unsigned long)fault_marker);
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t next_heartbeat_tick = HAL_GetTick();
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    HAL_IWDG_Refresh(&hiwdg);
    AT_Console_Process();

    if (HAL_GetTick() >= next_heartbeat_tick)
    {
      if (AT_Console_HeartbeatEnabled())
      {
      RESS_SensorData_t sensors;
      RESS_Sensors_GetLatest(&sensors);

      DS3231_Time_t now = {0};
      DS3231_GetTime(&rtc, &now);

      printf("[%04u-%02u-%02u %02u:%02u:%02u] V0=%ld.%02ldV V1=%ld.%02ldV I0=%ld.%02ldmA I1=%ld.%02ldmA valid=%d\r\n",
             now.year, now.month, now.date, now.hour, now.minute, now.second,
             (long)sensors.voltage_0_V, (long)((sensors.voltage_0_V - (long)sensors.voltage_0_V) * 100),
             (long)sensors.voltage_1_V, (long)((sensors.voltage_1_V - (long)sensors.voltage_1_V) * 100),
             (long)sensors.current_0_mA, (long)((sensors.current_0_mA - (long)sensors.current_0_mA) * 100),
             (long)sensors.current_1_mA, (long)((sensors.current_1_mA - (long)sensors.current_1_mA) * 100),
             sensors.valid);
      }

      next_heartbeat_tick = HAL_GetTick() + 1000U;
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/**
  * @brief EXTI callback -- fires when the ADS1115's ALERT/RDY pin (PE0)
  *        pulses after completing a single-shot conversion. The chip
  *        round-robins AIN0->AIN1->AIN2->AIN3 on its own; see ress_sensors.c.
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_0)
    {
        RESS_Sensors_OnConversionReady();
    }
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
