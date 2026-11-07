#include "esp_bridge.h"
#include "ress_config.h"
#include "ress_util.h"
#include <string.h>

static UART_HandleTypeDef *s_huart;
static char     s_tx_buf[400]; /* must stay valid while the IT transfer runs */
static uint32_t s_frames_sent;
static uint32_t s_frames_dropped;

void ESP_Bridge_Init(UART_HandleTypeDef *huart)
{
    s_huart = huart;
    s_frames_sent = 0;
    s_frames_dropped = 0;

    HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);

    RESS_LOG("ESP", "init: USART3 baud=%lu 8N1, IT transmit, every %u ms",
             (unsigned long)huart->Init.BaudRate, (unsigned)RESS_ESP_TX_INTERVAL_MS);
}

/* Appends "*XX\n" (XOR of chars after '$') to the frame and starts TX. */
static bool Finish(size_t len)
{
    uint8_t x = 0;
    for (size_t i = 1; i < len; i++)
    {
        x ^= (uint8_t)s_tx_buf[i];
    }

    int n = snprintf(&s_tx_buf[len], sizeof(s_tx_buf) - len, "*%02X\n", x);
    if (n < 0 || (size_t)n >= sizeof(s_tx_buf) - len)
    {
        RESS_LOG("ESP", "frame too long, dropped");
        s_frames_dropped++;
        return false;
    }
    len += (size_t)n;

    if (HAL_UART_Transmit_IT(s_huart, (uint8_t *)s_tx_buf, (uint16_t)len) != HAL_OK)
    {
        s_frames_dropped++;
        RESS_LOG("ESP", "TX start failed, dropped=%lu", (unsigned long)s_frames_dropped);
        return false;
    }

    s_frames_sent++;
    return true;
}

bool ESP_Bridge_SendTelemetry(const char *timestamp, const PowerMeter_Data_t *d)
{
    if (s_huart == NULL)
    {
        return false;
    }
    if (s_huart->gState != HAL_UART_STATE_READY)
    {
        s_frames_dropped++;
        RESS_LOG("ESP", "previous frame still sending, skipped");
        return false;
    }

    char fields[240];
    PowerMeter_FormatFields(fields, sizeof(fields), d);

    int n = snprintf(s_tx_buf, sizeof(s_tx_buf), "$TELE,%s,%s,%s", RESS_DEVICE_ID, timestamp, fields);
    if (n <= 0 || (size_t)n >= sizeof(s_tx_buf) - 8U)
    {
        RESS_LOG("ESP", "telemetry frame too long, dropped");
        return false;
    }

    if (!Finish((size_t)n))
    {
        return false;
    }

    RESS_LOG("ESP", "TX #%lu TELE (%d bytes) ts=%s", (unsigned long)s_frames_sent,
             n + 4, timestamp);
    return true;
}

bool ESP_Bridge_SendStatus(bool meter_online)
{
    if (s_huart == NULL || s_huart->gState != HAL_UART_STATE_READY)
    {
        return false;
    }

    int n = snprintf(s_tx_buf, sizeof(s_tx_buf), "$STAT,%s,%s", RESS_DEVICE_ID,
                     meter_online ? "ONLINE" : "OFFLINE");
    if (n <= 0 || !Finish((size_t)n))
    {
        return false;
    }

    RESS_LOG("ESP", "TX #%lu STAT %s", (unsigned long)s_frames_sent,
             meter_online ? "ONLINE" : "OFFLINE");
    return true;
}
