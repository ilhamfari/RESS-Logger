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

static void Print_I2CScan(void)
{
    static const struct { uint8_t addr7; const char *name; } known[] = {
        { ADS1115_ADDR  >> 1, "ADS1115 (ADC)"   },
        { DS3231_ADDR   >> 1, "DS3231 (RTC)"    },
        { AT24C256_ADDR >> 1, "AT24C256 (EEPROM)" },
    };

    printf("I2C1 scan:\r\n");
    uint8_t found_count = 0;

    for (uint8_t addr7 = 0x08U; addr7 <= 0x77U; addr7++)
    {
        HAL_IWDG_Refresh(&hiwdg); /* scan alone can run long enough to starve the ~512ms IWDG */
        if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr7 << 1), 2, 5) == HAL_OK)
        {
            const char *name = "unknown device";
            for (uint8_t k = 0; k < (sizeof(known) / sizeof(known[0])); k++)
            {
                if (known[k].addr7 == addr7)
                {
                    name = known[k].name;
                    break;
                }
            }
            printf("  0x%02X  %s\r\n", addr7, name);
            found_count++;
        }
    }

    if (found_count == 0U)
    {
        printf("  (nothing responded)\r\n");
    }
}

static void Print_SDCheck(void)
{
    printf("SD card (SPI2):\r\n");

    HAL_IWDG_Refresh(&hiwdg); /* card identification can take a while on its own */

    if (!SD_SPI_Init())
    {
        printf("  SD_SPI_Init FAILED -- no card responding\r\n");
        printf("  -> check card is seated, contacts are clean, and SD_CS/SPI2 wiring\r\n");
        return;
    }

    HAL_IWDG_Refresh(&hiwdg); /* f_mount can also take a while on a slow card */

    FRESULT fr = f_mount(&USERFatFS, USERPath, 1);
    if (fr != FR_OK)
    {
        printf("  card init OK but f_mount FAILED (FRESULT=%d) -- check filesystem/format (FAT32)\r\n", (int)fr);
        return;
    }

    HAL_IWDG_Refresh(&hiwdg);

    uint32_t capacity_mb = (uint32_t)(((uint64_t)SD_SPI_GetSectorCount() * SD_SPI_BLOCK_SIZE) / (1024U * 1024U));

    printf("  mounted OK, capacity ~%lu MB\r\n", (unsigned long)capacity_mb);
}

static void Print_FlashCheck(void)
{
    printf("W25Q16 flash:\r\n");

    W25Q16_Handle_t flash;
    W25Q16_Init(&flash, &hspi3, FLASH_CS_GPIO_Port, FLASH_CS_Pin);

    uint8_t id[3];
    if (W25Q16_ReadJedecId(&flash, id) != HAL_OK)
    {
        printf("  JEDEC ID read FAILED (SPI error)\r\n");
        return;
    }

    if (id[0] == 0x00U || id[0] == 0xFFU)
    {
        printf("  no response (ID=%02X %02X %02X) -- check wiring/CS\r\n", id[0], id[1], id[2]);
        return;
    }

    printf("  JEDEC ID: %02X %02X %02X", id[0], id[1], id[2]);
    if (id[0] == W25Q16_JEDEC_MANUFACTURER && id[1] == W25Q16_JEDEC_MEMTYPE)
    {
        printf(id[2] == W25Q16_JEDEC_CAPACITY ? " (Winbond, W25Q16)\r\n" : " (Winbond, capacity byte differs from W25Q16 -- update W25Q16_SIZE_BYTES if this is a different chip)\r\n");
    }
    else
    {
        printf(" (unrecognized manufacturer/type)\r\n");
    }
}

static void Print_RTCCheck(void)
{
    DS3231_Handle_t rtc;
    DS3231_Init(&rtc, &hi2c1);

    DS3231_Time_t now;
    if (DS3231_GetTime(&rtc, &now) != HAL_OK)
    {
        printf("RTC: not responding or oscillator-stop flag set (time not trustworthy)\r\n");
        return;
    }

    printf("RTC: %04u-%02u-%02u %02u:%02u:%02u\r\n",
           now.year, now.month, now.date, now.hour, now.minute, now.second);
}

static void Print_W5500Check_One(const char *label, W5500_Instance_t inst)
{
    W5500_Select(inst);

    uint8_t ver = getVERSIONR();
    if (ver != 0x04U)
    {
        printf("  %s: no response (VERSIONR=0x%02X) -- check wiring/CS/RST\r\n", label, ver);
        return;
    }

    int8_t link = wizphy_getphylink();
    uint8_t ip[4];
    getSIPR(ip);

    printf("  %s: chip OK (VER=0x04), link %s, IP %u.%u.%u.%u\r\n",
           label, (link == PHY_LINK_ON) ? "UP" : "DOWN",
           ip[0], ip[1], ip[2], ip[3]);
}

static void Print_W5500Check(void)
{
    printf("W5500 Ethernet:\r\n");
    Print_W5500Check_One("LAN (SPI1)", W5500_LAN);
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
    Print_I2CScan();
    Print_RTCCheck();
    Print_SDCheck();
    Print_FlashCheck();
    Print_W5500Check();
}

static void Cmd_Help(void)
{
    printf("Commands:\r\n");
    printf("  AT     -- toggle the 1Hz sensor heartbeat print ON/OFF\r\n");
    printf("  CHECK  -- scan I2C1 bus + RTC + SD card + W25Q16 flash + W5500\r\n");
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
    else if (strcmp(line, "LOG") == 0)
    {
        RESS_App_LogNow();
    }
    else if (strncmp(line, "LOGINT", 6) == 0)
    {
        unsigned minutes = 0;
        if (sscanf(line + 6, "%u", &minutes) == 1)
        {
            RESS_App_SetLogInterval((uint32_t)minutes);
        }
        else
        {
            printf("ERROR: usage: LOGINT <minutes>\r\n");
        }
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
