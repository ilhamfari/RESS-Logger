#include "inverters.h"
#include "ress_config.h"
#include "ress_util.h"
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Register maps (wire addresses, decimal unless noted)                       */
/* ------------------------------------------------------------------------- */

/* GoodWe GW10KT-DT / SDT G2, FC 0x03. */
#define GW_REG_SERIAL          512U
#define GW_SERIAL_REGS         8U
#define GW_REG_DEVICE_TYPE     528U
#define GW_DEVICE_TYPE_REGS    5U
#define GW_REG_MAIN            544U
#define GW_MAIN_REGS           23U /* 544..566 */
#define GW_REG_POWER_FACTOR    886U
#define GW_REG_EXTENDED        893U
#define GW_EXTENDED_REGS       4U  /* 893..896 */
#define GW_REG_LINE_VOLTAGES   30115U
#define GW_LINE_VOLTAGE_REGS   3U
#define GW_REG_INPUT_POWER     30137U
#define GW_INPUT_POWER_REGS    2U

/* Sungrow, FC 0x04, wire addresses as listed in the plan. */
#define SG_REG_ENERGY    0x138BU /* U32 word-little-endian, /10 kWh */
#define SG_REG_TEMP      0x138FU /* S16 /10 degC */
#define SG_REG_VPV       0x1392U /* U16 /10 V */
#define SG_REG_IPV       0x1393U /* U16 /10 A */
#define SG_REG_PPV       0x1398U /* U32 word-little-endian, W */
#define SG_REG_STATUS    0x32C7U /* U16 running state */
#define SG_REG_LOAD      0x32D0U /* S32 word-little-endian, W */
#define SG_REG_POUT      0x32EAU /* S32 word-little-endian, W */

static ModbusDevice_t s_gw_dev;
static ModbusDevice_t s_sg_dev;
static GoodWe_Data_t s_gw;
static Inverter_Data_t s_sg;

typedef enum
{
    GW_OPT_UNKNOWN = 0,
    GW_OPT_SUPPORTED,
    GW_OPT_UNSUPPORTED
} GoodWeOptionalState_t;

static GoodWeOptionalState_t s_line_voltage_state;
static GoodWeOptionalState_t s_input_power_state;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static bool ReadBlock(const ModbusDevice_t *dev, uint8_t fc, uint16_t start,
                      uint16_t count, uint8_t *buf, bool *abort_poll)
{
    uint8_t exc = 0;
    ModbusStatus_t st = Modbus_ReadRegisters(dev->slave_id, fc, start, count, buf, &exc);

    if (st == MB_OK)
    {
        return true;
    }

    if (st == MB_ERR_EXCEPTION)
    {
        RESS_LOG(dev->name, "reg %u: exception 0x%02X", (unsigned)start, exc);
    }
    else
    {
        RESS_LOG(dev->name, "reg %u: %s", (unsigned)start, Modbus_StatusStr(st));
    }

    if (st == MB_ERR_TIMEOUT || st == MB_ERR_UART)
    {
        *abort_poll = true; /* slave silent: do not wait on the remaining blocks */
    }
    return false;
}

static uint32_t U32_BE(const uint8_t *d, uint16_t idx)  /* high word first */
{
    return ((uint32_t)Modbus_U16(d, idx) << 16) | Modbus_U16(d, (uint16_t)(idx + 1U));
}

static int32_t S32_BE(const uint8_t *d, uint16_t idx)
{
    return (int32_t)U32_BE(d, idx);
}

static uint32_t U32_LE(const uint8_t *d, uint16_t idx)  /* low word first */
{
    return ((uint32_t)Modbus_U16(d, (uint16_t)(idx + 1U)) << 16) | Modbus_U16(d, idx);
}

static const char *GoodWeStatus(uint16_t m)
{
    switch (m)
    {
    case 0:  return "standby";
    case 1:  return "on_grid";
    case 2:  return "fault";
    default: return "unknown";
    }
}

static const char *GoodWeGridStatus(uint16_t m)
{
    return (m == 1U) ? "connected" : "disconnected";
}

static void DecodeAsciiRegisters(char *out, size_t out_size, const uint8_t *data, uint16_t regs)
{
    size_t n = (size_t)regs * 2U;
    if (n >= out_size)
    {
        n = out_size - 1U;
    }

    memcpy(out, data, n);
    while (n > 0U && (out[n - 1U] == '\0' || out[n - 1U] == ' ' || out[n - 1U] == (char)0xFF))
    {
        n--;
    }
    out[n] = '\0';
}

static ModbusStatus_t ReadOptionalBlock(const ModbusDevice_t *dev, uint16_t start,
                                        uint16_t count, uint8_t *buf)
{
    uint8_t exc = 0U;
    ModbusStatus_t st = Modbus_ReadRegisters(dev->slave_id, MB_FC_READ_HOLDING,
                                             start, count, buf, &exc);
    if (st == MB_ERR_EXCEPTION)
    {
        RESS_LOG(dev->name, "optional reg %u unsupported (exception 0x%02X)",
                 (unsigned)start, (unsigned)exc);
    }
    else if (st != MB_OK)
    {
        RESS_LOG(dev->name, "optional reg %u: %s", (unsigned)start, Modbus_StatusStr(st));
    }
    return st;
}

static const char *SungrowStatus(uint16_t s)
{
    switch (s)
    {
    case 0x0000:
    case 0x0040: return "on_grid";
    case 0x1000: return "off_grid";
    case 0x1400: return "standby";
    case 0x5500: return "fault";
    case 0x8000: return "off";
    default:     return "unknown";
    }
}

/* ------------------------------------------------------------------------- */
/* GoodWe                                                                     */
/* ------------------------------------------------------------------------- */

static bool PollGoodWe(const ModbusDevice_t *dev)
{
    uint8_t b[2U * GW_MAIN_REGS];
    bool abort_poll = false;
    GoodWe_Data_t d;

    memset(&d, 0, sizeof(d));
    snprintf(d.working_status, sizeof(d.working_status), "unknown");
    snprintf(d.grid_connection_status, sizeof(d.grid_connection_status), "disconnected");

    /* Identity changes rarely. Preserve a previously successful read and only
     * retry missing identity fields. */
    if (s_gw.has_device_serial)
    {
        d.has_device_serial = true;
        snprintf(d.device_serial, sizeof(d.device_serial), "%s", s_gw.device_serial);
    }
    else if (ReadBlock(dev, MB_FC_READ_HOLDING, GW_REG_SERIAL, GW_SERIAL_REGS, b, &abort_poll))
    {
        DecodeAsciiRegisters(d.device_serial, sizeof(d.device_serial), b, GW_SERIAL_REGS);
        d.has_device_serial = (d.device_serial[0] != '\0');
    }

    abort_poll = false;
    if (s_gw.has_device_type)
    {
        d.has_device_type = true;
        snprintf(d.device_type, sizeof(d.device_type), "%s", s_gw.device_type);
    }
    else if (ReadBlock(dev, MB_FC_READ_HOLDING, GW_REG_DEVICE_TYPE,
                       GW_DEVICE_TYPE_REGS, b, &abort_poll))
    {
        DecodeAsciiRegisters(d.device_type, sizeof(d.device_type), b, GW_DEVICE_TYPE_REGS);
        d.has_device_type = (d.device_type[0] != '\0');
    }

    abort_poll = false;
    bool main_ok = ReadBlock(dev, MB_FC_READ_HOLDING, GW_REG_MAIN,
                             GW_MAIN_REGS, b, &abort_poll);
    if (main_ok)
    {
        d.error_code = U32_BE(b, 0U);
        d.has_error_code = true;
        d.energy_total = (float)U32_BE(b, 2U) / 10.0f;
        d.has_energy_total = true;
        d.operating_hours_total = U32_BE(b, 4U);
        d.has_operating_hours = true;

        d.mppt1_voltage = (float)Modbus_U16(b, 6U) / 10.0f;
        d.mppt2_voltage = (float)Modbus_U16(b, 7U) / 10.0f;
        d.mppt1_current = (float)Modbus_U16(b, 8U) / 10.0f;
        d.mppt2_current = (float)Modbus_U16(b, 9U) / 10.0f;
        d.mppt1_power = d.mppt1_voltage * d.mppt1_current;
        d.mppt2_power = d.mppt2_voltage * d.mppt2_current;
        d.pv_power_total = d.mppt1_power + d.mppt2_power;
        d.has_mppt1_voltage = d.has_mppt2_voltage = true;
        d.has_mppt1_current = d.has_mppt2_current = true;
        d.has_mppt1_power = d.has_mppt2_power = true;
        d.has_pv_power_total = true;

        d.phase_a_voltage = (float)Modbus_U16(b, 10U) / 10.0f;
        d.phase_b_voltage = (float)Modbus_U16(b, 11U) / 10.0f;
        d.phase_c_voltage = (float)Modbus_U16(b, 12U) / 10.0f;
        d.phase_a_current = (float)Modbus_U16(b, 13U) / 10.0f;
        d.phase_b_current = (float)Modbus_U16(b, 14U) / 10.0f;
        d.phase_c_current = (float)Modbus_U16(b, 15U) / 10.0f;
        d.phase_a_frequency = (float)Modbus_U16(b, 16U) / 100.0f;
        d.phase_b_frequency = (float)Modbus_U16(b, 17U) / 100.0f;
        d.phase_c_frequency = (float)Modbus_U16(b, 18U) / 100.0f;
        d.grid_frequency = (d.phase_a_frequency + d.phase_b_frequency + d.phase_c_frequency) / 3.0f;
        d.has_phase_a_voltage = d.has_phase_b_voltage = d.has_phase_c_voltage = true;
        d.has_phase_a_current = d.has_phase_b_current = d.has_phase_c_current = true;
        d.has_phase_a_frequency = d.has_phase_b_frequency = d.has_phase_c_frequency = true;
        d.has_grid_frequency = true;

        d.active_power = (float)Modbus_U16(b, 19U);
        d.has_active_power = true;
        uint16_t work_mode = Modbus_U16(b, 20U);
        snprintf(d.working_status, sizeof(d.working_status), "%s", GoodWeStatus(work_mode));
        snprintf(d.grid_connection_status, sizeof(d.grid_connection_status), "%s",
                 GoodWeGridStatus(work_mode));
        d.has_working_status = true;
        d.has_grid_connection_status = true;
        d.inverter_temperature = (float)Modbus_U16(b, 21U) / 10.0f;
        d.has_inverter_temperature = true;
        d.energy_today = (float)Modbus_U16(b, 22U) / 10.0f;
        d.has_energy_today = true;
    }

    abort_poll = false;
    if (ReadBlock(dev, MB_FC_READ_HOLDING, GW_REG_POWER_FACTOR, 1U, b, &abort_poll))
    {
        d.power_factor = (float)Modbus_U16(b, 0U) / 1000.0f;
        d.has_power_factor = true;
    }

    abort_poll = false;
    if (ReadBlock(dev, MB_FC_READ_HOLDING, GW_REG_EXTENDED,
                  GW_EXTENDED_REGS, b, &abort_poll))
    {
        /* Raw / 1000 kVar is numerically equal to raw var. */
        d.reactive_power = (float)S32_BE(b, 0U);
        d.insulation_resistance = (float)Modbus_U16(b, 2U);
        d.leakage_current = (float)Modbus_U16(b, 3U);
        d.has_reactive_power = true;
        d.has_insulation_resistance = true;
        d.has_leakage_current = true;
    }

    if (s_line_voltage_state != GW_OPT_UNSUPPORTED)
    {
        ModbusStatus_t st = ReadOptionalBlock(dev, GW_REG_LINE_VOLTAGES,
                                              GW_LINE_VOLTAGE_REGS, b);
        if (st == MB_OK)
        {
            s_line_voltage_state = GW_OPT_SUPPORTED;
            d.line_ab_voltage = (float)Modbus_U16(b, 0U) / 10.0f;
            d.line_bc_voltage = (float)Modbus_U16(b, 1U) / 10.0f;
            d.line_ca_voltage = (float)Modbus_U16(b, 2U) / 10.0f;
            d.has_line_ab_voltage = d.has_line_bc_voltage = d.has_line_ca_voltage = true;
        }
        else if (st == MB_ERR_EXCEPTION)
        {
            s_line_voltage_state = GW_OPT_UNSUPPORTED;
        }
    }

    if (s_input_power_state != GW_OPT_UNSUPPORTED)
    {
        ModbusStatus_t st = ReadOptionalBlock(dev, GW_REG_INPUT_POWER,
                                              GW_INPUT_POWER_REGS, b);
        if (st == MB_OK)
        {
            s_input_power_state = GW_OPT_SUPPORTED;
            /* Datasheet: S32, gain 1000, unit kW => normalized value is W. */
            d.pv_power_total = (float)S32_BE(b, 0U);
            d.has_pv_power_total = true;
        }
        else if (st == MB_ERR_EXCEPTION)
        {
            s_input_power_state = GW_OPT_UNSUPPORTED;
        }
    }

    bool any = main_ok || d.has_power_factor || d.has_reactive_power;
    if (any)
    {
        d.online = true;
        d.update_tick = HAL_GetTick();
        s_gw = d;
        RESS_LOG("GOODWE", "status=%s Pac=%.0fW Ppv=%.0fW Eday=%.1fkWh Etot=%.1fkWh",
                 d.working_status, (double)d.active_power, (double)d.pv_power_total,
                 (double)d.energy_today, (double)d.energy_total);
    }
    return any;
}

void GoodWe_Init(uint8_t slave_id, uint32_t poll_interval_ms)
{
    memset(&s_gw, 0, sizeof(s_gw));
    snprintf(s_gw.working_status, sizeof(s_gw.working_status), "unknown");
    snprintf(s_gw.grid_connection_status, sizeof(s_gw.grid_connection_status), "disconnected");
    s_line_voltage_state = GW_OPT_UNKNOWN;
    s_input_power_state = GW_OPT_UNKNOWN;

    s_gw_dev.name = "GOODWE";
    s_gw_dev.slave_id = slave_id;
    s_gw_dev.dev_type = DEVICE_TYPE_INVERTER;
    s_gw_dev.format = MB_FMT_8N2;
    s_gw_dev.poll_interval_ms = poll_interval_ms;
    s_gw_dev.offline_retry_ms = RESS_INVERTER_OFFLINE_RETRY_MS;
    s_gw_dev.enabled = true;
    s_gw_dev.poll = PollGoodWe;

    RESS_LOG("GOODWE", "init: GW10KT-DT slave=%u 9600 8N2, main regs 544..566", slave_id);
    (void)Modbus_RegisterDevice(&s_gw_dev);
}

void GoodWe_SetEnabled(bool enabled)
{
    s_gw_dev.enabled = enabled;
    s_gw_dev.has_polled = false;
    s_gw_dev.fail_streak = 0U;

    if (!enabled)
    {
        s_gw_dev.is_online = false;
        s_gw.online = false;
    }

    RESS_LOG("GOODWE", "acquisition %s", enabled ? "ENABLED" : "DISABLED");
}

bool GoodWe_IsEnabled(void)
{
    return s_gw_dev.enabled;
}

void GoodWe_GetLatest(GoodWe_Data_t *out)
{
    *out = s_gw;
    out->online = s_gw_dev.is_online && s_gw.update_tick != 0U;
}

/* ------------------------------------------------------------------------- */
/* Sungrow                                                                    */
/* ------------------------------------------------------------------------- */

static bool PollSungrow(const ModbusDevice_t *dev)
{
    uint8_t b[2U * 9U];
    bool abort_poll = false;
    Inverter_Data_t d;

    memset(&d, 0, sizeof(d));
    d.status = "unknown";

    /* 0x138B..0x1393 (9 regs): energy(2) .. temp .. Vpv Ipv */
    if (ReadBlock(dev, MB_FC_READ_INPUT, SG_REG_ENERGY, (uint16_t)(SG_REG_IPV - SG_REG_ENERGY + 1U), b,
                  &abort_poll))
    {
        d.energy_kwh = (float)U32_LE(b, 0) / 10.0f;
        d.ok_energy = true;
        d.temp = (float)(int16_t)Modbus_U16(b, (uint16_t)(SG_REG_TEMP - SG_REG_ENERGY)) / 10.0f;
        d.ok_temp = true;
        d.vpv = (float)Modbus_U16(b, (uint16_t)(SG_REG_VPV - SG_REG_ENERGY)) / 10.0f;
        d.ipv = (float)Modbus_U16(b, (uint16_t)(SG_REG_IPV - SG_REG_ENERGY)) / 10.0f;
        d.ok_pv = true;
    }

    if (!abort_poll && ReadBlock(dev, MB_FC_READ_INPUT, SG_REG_PPV, 2, b, &abort_poll))
    {
        d.ppv = (float)U32_LE(b, 0);
        d.ok_ppv = true;
    }

    if (!abort_poll && ReadBlock(dev, MB_FC_READ_INPUT, SG_REG_STATUS, 1, b, &abort_poll))
    {
        d.status = SungrowStatus(Modbus_U16(b, 0));
        d.ok_status = true;
    }

    if (!abort_poll && ReadBlock(dev, MB_FC_READ_INPUT, SG_REG_LOAD, 2, b, &abort_poll))
    {
        int32_t v = (int32_t)U32_LE(b, 0);
        d.pload = (v < 0) ? 0.0f : (float)v; /* clamp >= 0 */
        d.ok_load = true;
    }

    if (!abort_poll && ReadBlock(dev, MB_FC_READ_INPUT, SG_REG_POUT, 2, b, &abort_poll))
    {
        d.pout = (float)(int32_t)U32_LE(b, 0);
        d.ok_pout = true;
    }

    bool any = d.ok_energy || d.ok_ppv || d.ok_status || d.ok_pout;
    if (any)
    {
        d.online = true;
        d.update_tick = HAL_GetTick();
        s_sg = d;

        char v[12], i[12], p[12], e[14], t[12];
        RESS_FmtFixed(v, sizeof(v), d.vpv, 1);
        RESS_FmtFixed(i, sizeof(i), d.ipv, 1);
        RESS_FmtFixed(p, sizeof(p), d.pout, 0);
        RESS_FmtFixed(e, sizeof(e), d.energy_kwh, 1);
        RESS_FmtFixed(t, sizeof(t), d.temp, 1);
        RESS_LOG("SUNGROW", "Vpv=%s V Ipv=%s A Pout=%s W E=%s kWh T=%s C status=%s", v, i, p, e, t,
                 d.status);
    }
    return any;
}

void Sungrow_Init(uint8_t slave_id, uint32_t poll_interval_ms)
{
    memset(&s_sg, 0, sizeof(s_sg));
    s_sg.status = "unknown";

    s_sg_dev.name = "SUNGROW";
    s_sg_dev.slave_id = slave_id;
    s_sg_dev.dev_type = DEVICE_TYPE_INVERTER;
    s_sg_dev.format = MB_FMT_8N1;
    s_sg_dev.poll_interval_ms = poll_interval_ms;
    s_sg_dev.offline_retry_ms = RESS_INVERTER_OFFLINE_RETRY_MS;
    s_sg_dev.enabled = true;
    s_sg_dev.poll = PollSungrow;

    RESS_LOG("SUNGROW", "init: slave=%u (FC 0x04)", slave_id);
    (void)Modbus_RegisterDevice(&s_sg_dev);
}

void Sungrow_GetLatest(Inverter_Data_t *out)
{
    *out = s_sg;
    out->online = s_sg_dev.is_online && s_sg.update_tick != 0U;
}

int GoodWe_FormatFields(char *buf, size_t size, const GoodWe_Data_t *d)
{
    char f[GOODWE_FIELD_COUNT][24];
    size_t i = 0U;
    bool on = d->online;

    #define GW_TEXT(ok, value) do { snprintf(f[i++], sizeof(f[0]), (on && (ok)) ? "%s" : "NA", (value)); } while (0)
    #define GW_U32(ok, value)  do { if (on && (ok)) snprintf(f[i], sizeof(f[0]), "%lu", (unsigned long)(value)); else strcpy(f[i], "NA"); i++; } while (0)
    #define GW_FLOAT(ok, value, dec) do { if (on && (ok)) RESS_FmtFixed(f[i], sizeof(f[0]), (value), (dec)); else strcpy(f[i], "NA"); i++; } while (0)

    GW_TEXT(d->has_device_serial, d->device_serial);
    GW_TEXT(d->has_device_type, d->device_type);
    GW_U32(d->has_error_code, d->error_code);
    GW_U32(d->has_operating_hours, d->operating_hours_total);
    GW_TEXT(d->has_working_status, d->working_status);
    GW_TEXT(d->has_grid_connection_status, d->grid_connection_status);
    GW_FLOAT(d->has_inverter_temperature, d->inverter_temperature, 1);

    GW_FLOAT(d->has_mppt1_voltage, d->mppt1_voltage, 1);
    GW_FLOAT(d->has_mppt1_current, d->mppt1_current, 1);
    GW_FLOAT(d->has_mppt1_power, d->mppt1_power, 1);
    GW_FLOAT(d->has_mppt2_voltage, d->mppt2_voltage, 1);
    GW_FLOAT(d->has_mppt2_current, d->mppt2_current, 1);
    GW_FLOAT(d->has_mppt2_power, d->mppt2_power, 1);
    GW_FLOAT(d->has_pv_power_total, d->pv_power_total, 1);
    GW_FLOAT(d->has_insulation_resistance, d->insulation_resistance, 1);
    GW_FLOAT(d->has_leakage_current, d->leakage_current, 1);

    GW_FLOAT(d->has_active_power, d->active_power, 0);
    GW_FLOAT(d->has_reactive_power, d->reactive_power, 0);
    GW_FLOAT(d->has_power_factor, d->power_factor, 3);
    GW_FLOAT(d->has_phase_a_voltage, d->phase_a_voltage, 1);
    GW_FLOAT(d->has_phase_b_voltage, d->phase_b_voltage, 1);
    GW_FLOAT(d->has_phase_c_voltage, d->phase_c_voltage, 1);
    GW_FLOAT(d->has_phase_a_current, d->phase_a_current, 1);
    GW_FLOAT(d->has_phase_b_current, d->phase_b_current, 1);
    GW_FLOAT(d->has_phase_c_current, d->phase_c_current, 1);
    GW_FLOAT(d->has_phase_a_frequency, d->phase_a_frequency, 2);
    GW_FLOAT(d->has_phase_b_frequency, d->phase_b_frequency, 2);
    GW_FLOAT(d->has_phase_c_frequency, d->phase_c_frequency, 2);
    GW_FLOAT(d->has_grid_frequency, d->grid_frequency, 2);
    GW_FLOAT(d->has_line_ab_voltage, d->line_ab_voltage, 1);
    GW_FLOAT(d->has_line_bc_voltage, d->line_bc_voltage, 1);
    GW_FLOAT(d->has_line_ca_voltage, d->line_ca_voltage, 1);
    GW_FLOAT(d->has_energy_total, d->energy_total, 1);
    GW_FLOAT(d->has_energy_today, d->energy_today, 1);

    #undef GW_TEXT
    #undef GW_U32
    #undef GW_FLOAT

    size_t pos = 0U;
    for (i = 0U; i < GOODWE_FIELD_COUNT; i++)
    {
        int n = snprintf(&buf[pos], (pos < size) ? size - pos : 0U,
                         (i == 0U) ? "%s" : ",%s", f[i]);
        if (n < 0 || pos + (size_t)n >= size)
        {
            return -1;
        }
        pos += (size_t)n;
    }
    return (int)pos;
}

void GoodWe_PrintDebug(const GoodWe_Data_t *d)
{
    if (!d->online)
    {
        RESS_LOG("GOODWE", "offline / no data");
        return;
    }
    RESS_LOG("GOODWE", "serial=%s type=%s status=%s grid=%s error=%lu hours=%lu",
             d->has_device_serial ? d->device_serial : "NA",
             d->has_device_type ? d->device_type : "NA",
             d->has_working_status ? d->working_status : "NA",
             d->has_grid_connection_status ? d->grid_connection_status : "NA",
             (unsigned long)d->error_code, (unsigned long)d->operating_hours_total);
    RESS_LOG("GOODWE", "MPPT1=%.1fV %.1fA %.1fW MPPT2=%.1fV %.1fA %.1fW PV=%.1fW AC=%.0fW",
             (double)d->mppt1_voltage, (double)d->mppt1_current, (double)d->mppt1_power,
             (double)d->mppt2_voltage, (double)d->mppt2_current, (double)d->mppt2_power,
             (double)d->pv_power_total, (double)d->active_power);
}

int Inverter_FormatFields(char *buf, size_t size, const Inverter_Data_t *d)
{
    char t[8][16];
    bool on = d->online;

    #define FMT(i, ok, val, dec)         do { if (on && (ok)) { RESS_FmtFixed(t[i], sizeof(t[i]), (val), (dec)); }              else { strcpy(t[i], "NA"); } } while (0)

    FMT(0, d->ok_pv,     d->vpv,        1);
    FMT(1, d->ok_pv,     d->ipv,        1);
    FMT(2, d->ok_ppv,    d->ppv,        0);
    FMT(3, d->ok_energy, d->energy_kwh, 1);
    FMT(4, d->ok_pout,   d->pout,       0);
    FMT(5, d->ok_load,   d->pload,      0);
    FMT(7, d->ok_temp,   d->temp,       1);
    #undef FMT

    if (on && d->ok_status && d->status != NULL)
    {
        snprintf(t[6], sizeof(t[6]), "%s", d->status);
    }
    else
    {
        strcpy(t[6], "NA");
    }

    return snprintf(buf, size, "%s,%s,%s,%s,%s,%s,%s,%s", t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
}

void Inverter_PrintDebug(const char *name, const Inverter_Data_t *d)
{
    if (!d->online)
    {
        RESS_LOG(name, "offline / no data");
        return;
    }

    char v[12], i[12], pp[12], po[12], pl[12], e[14], t[12];
    RESS_FmtFixed(v, sizeof(v), d->vpv, 1);
    RESS_FmtFixed(i, sizeof(i), d->ipv, 1);
    RESS_FmtFixed(pp, sizeof(pp), d->ppv, 0);
    RESS_FmtFixed(po, sizeof(po), d->pout, 0);
    RESS_FmtFixed(pl, sizeof(pl), d->pload, 0);
    RESS_FmtFixed(e, sizeof(e), d->energy_kwh, 1);
    RESS_FmtFixed(t, sizeof(t), d->temp, 1);
    RESS_LOG(name, "Vpv=%s Ipv=%s Ppv=%s Pout=%s Pload=%s E=%s kWh T=%s status=%s age=%lu ms", v, i, pp, po,
             pl, e, t, d->status, (unsigned long)(HAL_GetTick() - d->update_tick));
}
