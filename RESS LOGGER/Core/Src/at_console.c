#include "at_console.h"
#include "i2c.h"
#include "iwdg.h"
#include "ads1115.h"
#include "ds3231.h"
#include "at24c256.h"
#include "fatfs.h"
#include "sd_spi.h"
#include "spi.h"
#include "w25q16.h"
#include "w5500_port.h"
#include "ress_app.h"
#include "ress_config.h"
#include "datalogger.h"
#include "relay.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

#define RX_RINGBUF_SIZE   256U  /* power of 2 */
#define LINE_BUF_SIZE     96U

static uint8_t  s_rx_ring[RX_RINGBUF_SIZE];
static volatile uint16_t s_rx_head; /* written by ISR (producer) */
static volatile uint16_t s_rx_tail; /* written by main loop (consumer) */

static char     s_line_buf[LINE_BUF_SIZE];
static uint16_t s_line_len;
static volatile uint32_t s_last_rx_tick;

#define IDLE_FLUSH_MS   150U /* dispatch a pending line if the terminal never sends CR/LF */

void AT_Console_OnUsbRx(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        uint16_t next_head = (uint16_t)((s_rx_head + 1U) % RX_RINGBUF_SIZE);
        if (next_head != s_rx_tail) /* drop byte silently if ring is full */
        {
            s_rx_ring[s_rx_head] = data[i];
            s_rx_head = next_head;
        }
    }
    if (len > 0U)
    {
        s_last_rx_tick = HAL_GetTick();
    }
}

static bool RingBuf_Pop(uint8_t *out_byte)
{
    if (s_rx_tail == s_rx_head)
    {
        return false;
    }
    *out_byte = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1U) % RX_RINGBUF_SIZE);
    return true;
}

#define RULE_THICK  "============================================================\r\n"
#define RULE_THIN   "------------------------------------------------------------\r\n"

static const char *OnOff(bool v)
{
    return v ? "ON" : "OFF";
}

static void Report_Devices(void)
{
    bool goodwe;
    RESS_App_GetDeviceStatus(NULL, &goodwe, NULL);

    uint32_t ms = HAL_GetTick();
    uint32_t sec = ms / 1000U;

    printf("\r\n[1] SYSTEM & DEVICE INFO:\r\n");
    printf("  Device ID         : %s\r\n", RESS_DEVICE_ID);
    printf("  Firmware Version  : %s (STM32F407VGT6 @ %lu MHz)\r\n", RESS_FW_VERSION,
           (unsigned long)(HAL_RCC_GetSysClockFreq() / 1000000UL));
    printf("  System Uptime     : %luh %lum %lus (%lu ms)\r\n", (unsigned long)(sec / 3600U),
           (unsigned long)((sec / 60U) % 60U), (unsigned long)(sec % 60U), (unsigned long)ms);
    printf("  Power Meter       : DISABLED\r\n");
    printf("  GoodWe Inverter   : %s (Modbus ID %u)\r\n", goodwe ? "ONLINE" : "OFFLINE", (unsigned)RESS_GOODWE_SLAVE_ID);
    printf("  Sungrow Inverter  : DISABLED\r\n");
    HAL_IWDG_Refresh(&hiwdg);

    /* W25Q16 */
    W25Q16_Handle_t flash;
    uint8_t id[3] = { 0, 0, 0 };
    W25Q16_Init(&flash, &hspi3, FLASH_CS_GPIO_Port, FLASH_CS_Pin);
    if (W25Q16_ReadJedecId(&flash, id) != HAL_OK)
    {
        printf("  W25Q16 Flash      : ERROR (SPI read failed)\r\n");
    }
    else if (id[0] == 0x00U || id[0] == 0xFFU)
    {
        printf("  W25Q16 Flash      : NOT DETECTED (ID=%02X %02X %02X)\r\n", id[0], id[1], id[2]);
    }
    else if (id[0] == W25Q16_JEDEC_MANUFACTURER && id[1] == W25Q16_JEDEC_MEMTYPE)
    {
        printf("  W25Q16 Flash      : OK (Winbond %lu MB, JEDEC: %02X %02X %02X%s)\r\n",
               (unsigned long)(W25Q16_SIZE_BYTES / (1024UL * 1024UL)), id[0], id[1], id[2],
               (id[2] == W25Q16_JEDEC_CAPACITY) ? "" : ", capacity differs from W25Q16");
    }
    else
    {
        printf("  W25Q16 Flash      : UNKNOWN CHIP (JEDEC: %02X %02X %02X)\r\n", id[0], id[1], id[2]);
    }
    HAL_IWDG_Refresh(&hiwdg);

    /* W5500 */
    W5500_Select(W5500_LAN);
    uint8_t ver = getVERSIONR();
    if (ver != 0x04U)
    {
        printf("  W5500 Ethernet    : NOT DETECTED (VERSIONR=0x%02X)\r\n", ver);
    }
    else
    {
        uint8_t ip[4];
        getSIPR(ip);
        printf("  W5500 Ethernet    : OK (Link: %s, IP: %u.%u.%u.%u)\r\n",
               (wizphy_getphylink() == PHY_LINK_ON) ? "UP" : "DOWN", ip[0], ip[1], ip[2], ip[3]);
    }
}

static void Report_Time(void)
{
    DS3231_Time_t t;
    bool valid;
    uint32_t age_s;

    (void)RESS_App_GetTimeInfo(&t, &valid);
    RessTimeSource_t src = RESS_App_GetTimeSource(&age_s);

    printf("\r\n[2] TIME INFO:\r\n");
    if (valid)
    {
        printf("  Current Time      : %04u-%02u-%02u %02u:%02u:%02u UTC\r\n", (unsigned)t.year, (unsigned)t.month,
               (unsigned)t.date, (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
    }
    else
    {
        printf("  Current Time      : (not set)\r\n");
    }

    if (!valid)
    {
        printf("  Time Status       : NOT VALID - Waiting for NTP\r\n");
    }
    else if (src == TIME_SRC_NTP)
    {
        printf("  Time Status       : VALID (Calculated via NTP from ESP32, synced %lu s ago)\r\n", (unsigned long)age_s);
    }
    else
    {
        printf("  Time Status       : VALID (set manually via SETTIME %lu s ago, NO NTP yet)\r\n", (unsigned long)age_s);
    }
    printf("  FatFs FAT Timestamp: 0x%08lX\r\n", (unsigned long)DataLogger_GetFatTime());
}

static void Report_SD(void)
{
    printf("\r\n[3] SD CARD STORAGE (SPI2):\r\n");
    HAL_IWDG_Refresh(&hiwdg);

    bool mounted = DataLogger_IsMounted();
    if (!mounted)
    {
        /* Not mounted by the logger yet: try now so the report is meaningful. */
        if (SD_SPI_Init())
        {
            HAL_IWDG_Refresh(&hiwdg);
            mounted = (f_mount(&USERFatFS, USERPath, 1) == FR_OK);
        }
    }

    if (!mounted)
    {
        printf("  Card Status       : NOT DETECTED (check card, FAT32 format, SPI2 wiring)\r\n");
        printf("  Total Capacity    : -\r\n");
    }
    else
    {
        uint32_t mb = (uint32_t)(((uint64_t)SD_SPI_GetSectorCount() * SD_SPI_BLOCK_SIZE) / (1024U * 1024U));
        printf("  Card Status       : MOUNTED\r\n");
        printf("  Total Capacity    : ~%lu MB\r\n", (unsigned long)mb);
    }
    printf("  Log Directory     : %sGWLOG2/\r\n", USERPath);
    printf("  Pending Queue     : %sGWPEND/ (persistent)\r\n", USERPath);
}

static void Report_SDStatus(void)
{
    printf("\r\n[4] SD WRITE STATUS:\r\n");
    printf("  Total Rows Written: %lu rows\r\n", (unsigned long)DataLogger_GetRowsWritten());
    printf("  Log Trigger       : every telemetry request ($REQ,TELE) from ESP32\r\n");

    printf("\r\n[5] SD TRANSMISSION STATUS (MQTT Store-and-Forward):\r\n");
    printf("  Rows Confirmed MQTT: %lu rows\r\n", (unsigned long)DataLogger_GetRowsSent());
    printf("  Rows Pending Ack   : %u rows on SD\r\n", (unsigned)DataLogger_GetPendingCount());
}

static void Report_Relays(void)
{
    printf("\r\n[6] RELAY OUTPUTS (PE7 - PE10):\r\n");
    for (uint8_t ch = 1; ch <= RESS_RELAY_COUNT; ch++)
    {
        printf("  Relay %u (PE%u)     : %s\r\n", (unsigned)ch, (unsigned)(6U + ch), OnOff(Relay_Get(ch)));
    }

    uint8_t mask = Relay_GetMask();
    uint8_t saved = 0;
    int st = Relay_FlashStatus(&saved);

    printf("  Bitmask           : 0x%X\r\n", (unsigned)mask);
    if (st < 0)
    {
        printf("  W25Q16 Persistent : ERROR (flash read failed)\r\n");
    }
    else if (st == 0)
    {
        printf("  W25Q16 Persistent : NO SAVED STATE (Sector 511: 0x%06lX is blank/invalid)\r\n",
               (unsigned long)RELAY_FLASH_ADDR);
    }
    else if (saved == mask)
    {
        printf("  W25Q16 Persistent : SYNCED (Sector 511: 0x%06lX)\r\n", (unsigned long)RELAY_FLASH_ADDR);
    }
    else
    {
        printf("  W25Q16 Persistent : OUT OF SYNC (flash mask=0x%X, Sector 511: 0x%06lX)\r\n", (unsigned)saved,
               (unsigned long)RELAY_FLASH_ADDR);
    }
}

static volatile bool s_heartbeat_enabled = true;

bool AT_Console_HeartbeatEnabled(void)
{
    return s_heartbeat_enabled;
}

static void Cmd_At(void)
{
    s_heartbeat_enabled = !s_heartbeat_enabled;
    printf("OK heartbeat %s\r\n", s_heartbeat_enabled ? "ON" : "OFF");
}

static void Cmd_Check(void)
{
    printf("\r\n" RULE_THICK);
    printf("              RESS LOGGER - SYSTEM STATUS REPORT\r\n");
    printf(RULE_THICK);
    Report_Devices();
    HAL_IWDG_Refresh(&hiwdg);
    Report_Time();
    Report_SD();
    HAL_IWDG_Refresh(&hiwdg);
    Report_SDStatus();
    Report_Relays();
    HAL_IWDG_Refresh(&hiwdg);
    printf("\r\n" RULE_THICK);
}

static void Cmd_Help(void)
{
    printf("Commands:\r\n");
    printf("  AT     -- toggle the 1Hz sensor heartbeat print ON/OFF\r\n");
    printf("  CHECK  -- full status report (devices, time, SD, relays)\r\n");
    printf("  INV    -- latest GoodWe inverter values\r\n");
    printf("  RELAY n s -- set relay n (1..4) to s (0/1)\r\n");
    printf("  LOG    -- write one SD row now\r\n");
    printf("  SETTIME YYYY-MM-DD HH:MM:SS -- set the clock manually (normally from ESP32 NTP)\r\n");
    printf("  HELP   -- this message\r\n");
}

static void Dispatch(char *line)
{
    /* trim leading/trailing whitespace in place */
    while (*line == ' ' || *line == '\t')
    {
        line++;
    }
    size_t len = strlen(line);
    while (len > 0U && (line[len - 1U] == ' ' || line[len - 1U] == '\t'))
    {
        line[--len] = '\0';
    }
    if (len == 0U)
    {
        return;
    }

    for (size_t i = 0; i < len; i++)
    {
        line[i] = (char)toupper((unsigned char)line[i]);
    }

    if (strcmp(line, "AT") == 0)
    {
        Cmd_At();
    }
    else if (strcmp(line, "CHECK") == 0)
    {
        Cmd_Check();
    }
    else if (strcmp(line, "METER") == 0)
    {
        RESS_App_PrintMeter();
    }
    else if (strcmp(line, "INV") == 0)
    {
        RESS_App_PrintInverters();
    }
    else if (strncmp(line, "RELAY", 5) == 0)
    {
        unsigned ch, st;
        if (sscanf(line + 5, "%u %u", &ch, &st) == 2 && ch >= 1U && ch <= 4U && st <= 1U)
        {
            (void)RESS_App_SetRelay((uint8_t)ch, st != 0U);
        }
        else
        {
            printf("ERROR: usage: RELAY <1..4> <0|1>%s", "\r\n");
        }
    }
    else if (strcmp(line, "LOG") == 0)
    {
        RESS_App_LogNow();
    }
    else if (strncmp(line, "SETTIME", 7) == 0)
    {
        unsigned y, mo, d, h, mi, s;
        if (sscanf(line + 7, "%u-%u-%u %u:%u:%u", &y, &mo, &d, &h, &mi, &s) == 6)
        {
            (void)RESS_App_SetTime((uint16_t)y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)s);
        }
        else
        {
            printf("ERROR: usage: SETTIME YYYY-MM-DD HH:MM:SS\r\n");
        }
    }
    else if ((strcmp(line, "HELP") == 0) || (strcmp(line, "?") == 0))
    {
        Cmd_Help();
    }
    else
    {
        printf("ERROR: unknown command '%s' (try HELP)\r\n", line);
    }
}

void AT_Console_Process(void)
{
    uint8_t byte;
    bool got_byte = false;

    while (RingBuf_Pop(&byte))
    {
        got_byte = true;
        if (byte == '\r' || byte == '\n')
        {
            if (s_line_len > 0U)
            {
                s_line_buf[s_line_len] = '\0';
                Dispatch(s_line_buf);
                s_line_len = 0U;
            }
        }
        else if (s_line_len < (LINE_BUF_SIZE - 1U))
        {
            s_line_buf[s_line_len++] = (char)byte;
        }
        else
        {
            /* line too long -- drop it and resync on next terminator */
            s_line_len = 0U;
        }
    }

    /* Some terminals send raw text with no CR/LF at all -- if a line has
     * been sitting unterminated for a while with no new bytes arriving,
     * treat the pause as the terminator instead of waiting forever. */
    if (!got_byte && s_line_len > 0U && (HAL_GetTick() - s_last_rx_tick) >= IDLE_FLUSH_MS)
    {
        s_line_buf[s_line_len] = '\0';
        Dispatch(s_line_buf);
        s_line_len = 0U;
    }
}
