/**
  * @file    ress_config.h
  * @brief   Application-level configuration for the RESS V logger.
  */

#ifndef RESS_CONFIG_H
#define RESS_CONFIG_H

#define RESS_DEVICE_ID             "RESS01"

/* Power meter (Modbus RTU on USART1 / RS485, 9600 8E1) */
#define RESS_METER_SLAVE_ID        0x01U
#define RESS_METER_POLL_MS         1000U
#define RESS_MODBUS_BAUD           9600U

/* STM32 -> ESP32 telemetry on USART3 (115200 8N1) */
#define RESS_ESP_TX_INTERVAL_MS    2000U

/* RTC read period for timestamps */
#define RESS_RTC_REFRESH_MS        1000U

#endif /* RESS_CONFIG_H */
