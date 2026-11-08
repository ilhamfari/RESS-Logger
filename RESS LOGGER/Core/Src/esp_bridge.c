#include "esp_bridge.h"
#include "iwdg.h"
#include "ress_util.h"
#include <stdlib.h>
#include <string.h>

#define RX_RING_SIZE   512U   /* power of 2 */
#define LINE_MAX_LEN   160U
#define TX_BUF_SIZE    320U

static UART_HandleTypeDef  *s_huart;
static ESP_Handlers_t       s_h;

static uint8_t              s_rx_byte;
static uint8_t              s_ring[RX_RING_SIZE];
static volatile uint16_t    s_head;
static volatile uint16_t    s_tail;
static volatile uint32_t    s_overflow;

static char     s_line[LINE_MAX_LEN];
static uint16_t s_line_len;
static bool     s_in_frame;

static char     s_tx[TX_BUF_SIZE];

/* ------------------------------------------------------------------------- */
/* Checksum / framing                                                         */
/* ------------------------------------------------------------------------- */

static uint8_t Xor(const char *s, size_t len)
{
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++)
    {
        x ^= (uint8_t)s[i];
    }
    return x;
}

static bool SendFrame(const char *payload)
{
    int n = snprintf(s_tx, sizeof(s_tx), "$%s*%02X\r\n", payload,
                     (unsigned)Xor(payload, strlen(payload)));
    if (n <= 0 || (size_t)n >= sizeof(s_tx))
    {
        RESS_LOG("ESP", "frame too long, dropped");
        return false;
    }

    HAL_IWDG_Refresh(&hiwdg);
    if (HAL_UART_Transmit(s_huart, (uint8_t *)s_tx, (uint16_t)n, 100U) != HAL_OK)
    {
        RESS_LOG("ESP", "TX failed");
        return false;
    }

    /* strip CRLF for the debug print */
    RESS_LOG("ESP", "TX %.*s", n - 2, s_tx);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Receive path                                                               */
/* ------------------------------------------------------------------------- */

static void StartRx(void)
{
    (void)HAL_UART_Receive_IT(s_huart, &s_rx_byte, 1);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (s_huart != NULL && huart == s_huart)
    {
        uint16_t next = (uint16_t)((s_head + 1U) & (RX_RING_SIZE - 1U));
        if (next != s_tail)
        {
            s_ring[s_head] = s_rx_byte;
            s_head = next;
        }
        else
        {
            s_overflow++;
        }
        StartRx();
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (s_huart != NULL && huart == s_huart)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        StartRx();
    }
}

void ESP_Bridge_Init(UART_HandleTypeDef *huart, const ESP_Handlers_t *handlers)
{
    s_huart = huart;
    s_h = *handlers;
    s_head = s_tail = 0;
    s_line_len = 0;
    s_in_frame = false;

    HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
    StartRx();

    RESS_LOG("ESP", "init: USART3 baud=%lu 8N1, IT receive, waiting for $REQ/$CMD/$ACK",
             (unsigned long)huart->Init.BaudRate);
}

/* ------------------------------------------------------------------------- */
/* Frame dispatch                                                             */
/* ------------------------------------------------------------------------- */

static void HandleFrame(char *line)
{
    /* line = "<payload>*XX" (without leading '$' and trailing CRLF) */
    char *star = strrchr(line, '*');
    if (star == NULL || strlen(star) != 3U)
    {
        RESS_LOG("ESP", "RX bad frame (no checksum): %s", line);
        return;
    }

    unsigned rx_sum = 0;
    char *endp = NULL;
    rx_sum = (unsigned)strtoul(star + 1, &endp, 16);
    if (endp == NULL || *endp != '\0')
    {
        RESS_LOG("ESP", "RX bad checksum text: %s", line);
        return;
    }

    size_t plen = (size_t)(star - line);
    uint8_t calc = Xor(line, plen);
    if (calc != (uint8_t)rx_sum)
    {
        RESS_LOG("ESP", "RX checksum MISMATCH (got %02X, calc %02X): %.*s", rx_sum, (unsigned)calc,
                 (int)plen, line);
        return;
    }
    *star = '\0';

    RESS_LOG("ESP", "RX %s", line);

    char *tok[12];
    size_t nt = 0;
    char *p = line;
    while (nt < 12U)
    {
        tok[nt++] = p;
        char *comma = strchr(p, ',');
        if (comma == NULL)
        {
            break;
        }
        *comma = '\0';
        p = comma + 1;
    }

    if (strcmp(tok[0], "REQ") == 0 && nt >= 2U && strcmp(tok[1], "TELE") == 0)
    {
        if (s_h.on_req_tele != NULL)
        {
            uint32_t scheduled_epoch = (nt >= 3U) ? (uint32_t)strtoul(tok[2], NULL, 10) : 0U;
            s_h.on_req_tele(scheduled_epoch);
        }
    }
    else if (strcmp(tok[0], "CMD") == 0 && nt >= 4U && strcmp(tok[1], "RELAY") == 0)
    {
        int ch = atoi(tok[2]);
        int st = atoi(tok[3]);
        bool st_ok = (strcmp(tok[3], "0") == 0) || (strcmp(tok[3], "1") == 0);

        if (ch < 1 || ch > 4 || !st_ok || s_h.on_cmd_relay == NULL)
        {
            RESS_LOG("ESP", "relay command rejected (ch=%s state=%s)", tok[2], tok[3]);
            SendFrame("ERR,RELAY,INVALID");
            return;
        }

        int result = s_h.on_cmd_relay((uint8_t)ch, (uint8_t)st);
        if (result < 0)
        {
            SendFrame("ERR,RELAY,FAILED");
            return;
        }
        char ack[24];
        snprintf(ack, sizeof(ack), "ACK,RELAY,%d,%d", ch, result);
        SendFrame(ack);
    }
    else if (strcmp(tok[0], "CMD") == 0 && nt >= 3U && strcmp(tok[1], "GOODWE") == 0)
    {
        bool value_ok = (strcmp(tok[2], "0") == 0) || (strcmp(tok[2], "1") == 0);
        if (!value_ok || s_h.on_cmd_goodwe == NULL)
        {
            SendFrame("ERR,GOODWE,INVALID");
            return;
        }

        bool enabled = (tok[2][0] == '1');
        s_h.on_cmd_goodwe(enabled);
        SendFrame(enabled ? "ACK,GOODWE,1" : "ACK,GOODWE,0");
    }
    else if (strcmp(tok[0], "CMD") == 0 && nt >= 8U && strcmp(tok[1], "TIME") == 0)
    {
        uint16_t year = (uint16_t)atoi(tok[2]);
        uint8_t month = (uint8_t)atoi(tok[3]);
        uint8_t date  = (uint8_t)atoi(tok[4]);
        uint8_t hour  = (uint8_t)atoi(tok[5]);
        uint8_t min   = (uint8_t)atoi(tok[6]);
        uint8_t sec   = (uint8_t)atoi(tok[7]);
        uint32_t epoch = (nt >= 9U) ? (uint32_t)strtoul(tok[8], NULL, 10) : 0U;

        RESS_LOG("NTP", "RX $CMD,TIME from ESP32 -> %04u-%02u-%02u %02u:%02u:%02u UTC (epoch=%lu)",
                 (unsigned)year, (unsigned)month, (unsigned)date,
                 (unsigned)hour, (unsigned)min, (unsigned)sec, (unsigned long)epoch);

        if (s_h.on_cmd_time != NULL && s_h.on_cmd_time(year, month, date, hour, min, sec, epoch))
        {
            RESS_LOG("NTP", "Time synchronization VALID & APPLIED successfully! -> Sending ACK");
            SendFrame("ACK,TIME,SUCCESS");
        }
        else
        {
            RESS_LOG("NTP", "Time synchronization REJECTED or FAILED! -> Sending ERR");
            SendFrame("ERR,TIME,FAILED");
        }
    }
    else if (strcmp(tok[0], "ACK") == 0 && nt >= 4U && strcmp(tok[1], "TELE") == 0)
    {
        uint32_t seq = (uint32_t)strtoul(tok[2], NULL, 10);
        bool ok = (strcmp(tok[3], "SUCCESS") == 0);
        if (s_h.on_ack_tele != NULL)
        {
            s_h.on_ack_tele(seq, ok);
        }
    }
    else
    {
        RESS_LOG("ESP", "RX unknown frame: %s", tok[0]);
    }
}

void ESP_Bridge_Process(void)
{
    while (s_tail != s_head)
    {
        char c = (char)s_ring[s_tail];
        s_tail = (uint16_t)((s_tail + 1U) & (RX_RING_SIZE - 1U));

        if (c == '$')
        {
            s_in_frame = true;
            s_line_len = 0;
        }
        else if (!s_in_frame || c == '\r')
        {
            continue;
        }
        else if (c == '\n')
        {
            s_line[s_line_len] = '\0';
            s_in_frame = false;
            HandleFrame(s_line);
        }
        else if (s_line_len < (LINE_MAX_LEN - 1U))
        {
            s_line[s_line_len++] = c;
        }
        else
        {
            s_in_frame = false; /* too long: drop until next '$' */
            RESS_LOG("ESP", "RX frame too long, dropped");
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Telemetry batch                                                            */
/* ------------------------------------------------------------------------- */

bool ESP_Bridge_SendReqTime(void)
{
    if (s_huart == NULL)
    {
        return false;
    }
    return SendFrame("REQ,TIME");
}

bool ESP_Bridge_SendSnapshot(const RessSnapshot_t *s)
{
    char fields[200];
    char payload[240];
    bool ok = true;

    if (s_huart == NULL)
    {
        return false;
    }

    /* GoodWe */
    if (s->goodwe.online)
    {
        Inverter_FormatFields(fields, sizeof(fields), &s->goodwe);
        snprintf(payload, sizeof(payload), "DATA,GOODWE,%lu,%s", (unsigned long)s->seq, fields);
    }
    else
    {
        snprintf(payload, sizeof(payload), "DATA,GOODWE,%lu,NA", (unsigned long)s->seq);
    }
    ok &= SendFrame(payload);

    /* Relays */
    snprintf(payload, sizeof(payload), "DATA,RELAY,%lu,%u,%u,%u,%u", (unsigned long)s->seq,
             (unsigned)(s->relays & 1U), (unsigned)((s->relays >> 1) & 1U),
             (unsigned)((s->relays >> 2) & 1U), (unsigned)((s->relays >> 3) & 1U));
    ok &= SendFrame(payload);

    snprintf(payload, sizeof(payload), "DATA,END,%lu", (unsigned long)s->seq);
    ok &= SendFrame(payload);

    return ok;
}
