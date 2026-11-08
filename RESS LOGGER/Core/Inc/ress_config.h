/**
  * @file    ress_config.h
  * @brief   Application-level configuration for the RESS V logger.
  */

#ifndef RESS_CONFIG_H
#define RESS_CONFIG_H

#define RESS_DEVICE_ID             "RESS01"
#define RESS_FW_VERSION            "0.1.0"

/* Modbus RTU bus (USART1 / RS485, auto-direction, 9600 bps).
 * Meter is 8E1; both inverters are 8N1 -- the driver switches per device. */
#define RESS_MODBUS_BAUD           9600U

#define RESS_METER_SLAVE_ID        0x01U
#define RESS_METER_POLL_MS         1000U

#define RESS_GOODWE_SLAVE_ID       0x02U
#define RESS_GOODWE_POLL_MS        5000U
/* 0 = Non-MT / SDTG2 register map, 1 = MT / SMT register map */
#define RESS_GOODWE_MT             0

#define RESS_SUNGROW_SLAVE_ID      0x03U
#define RESS_SUNGROW_POLL_MS       5000U

/* An inverter that stops answering is retried this slowly so it cannot
 * starve the meter (each silent attempt costs a 200 ms timeout). */
#define RESS_INVERTER_OFFLINE_RETRY_MS  15000U

/* RTC read period for timestamps */
#define RESS_RTC_REFRESH_MS        1000U

/* Number of relay channels (PE7..PE10) */
#define RESS_RELAY_COUNT           4U

#endif /* RESS_CONFIG_H */
