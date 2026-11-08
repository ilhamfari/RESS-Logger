#include "inverters.h"
#include "ress_config.h"
#include "ress_util.h"
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Register maps (wire addresses, decimal unless noted)                       */
/* ------------------------------------------------------------------------- */

/* GoodWe, FC 0x03. Two layouts, selected by RESS_GOODWE_MT in ress_config.h. */
#if RESS_GOODWE_MT
#define GW_REG_ENERGY    786U  /* U32 /10 kWh */
#define GW_REG_VPV       768U  /* U16 /10 V   */
#define GW_REG_IPV       770U  /* U16 /10 A   */
#define GW_REG_POUT      850U  /* U32 W (feeding power) */
#define GW_REG_STATUS    782U  /* U16 work mode */
#define GW_REG_TEMP      783U  /* U16 /10 degC  */
#else
#define GW_REG_ENERGY    546U  /* U32 /10 kWh */
#define GW_REG_VPV       550U  /* U16 /10 V   */
#define GW_REG_IPV       552U  /* U16 /10 A   */
#define GW_REG_POUT      563U  /* U16 W (Pac L) */
#define GW_REG_STATUS    564U  /* U16 work mode */
#define GW_REG_TEMP      565U  /* U16 /10 degC  */
#endif

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
static Inverter_Data_t s_gw;
static Inverter_Data_t s_sg;

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
    uint8_t b[8];
    bool abort_poll = false;
    Inverter_Data_t d;

    memset(&d, 0, sizeof(d));
    d.status = "unknown";

    /* lifetime energy: U32, 2 regs */
    if (ReadBlock(dev, MB_FC_READ_HOLDING, GW_REG_ENERGY, 2, b, &abort_poll))
    {
        d.energy_kwh = (float)U32_BE(b, 0) / 10.0f;
        d.ok_energy = true;
    }

    /* Vpv and Ipv (3 regs cover both on the Non-MT map; MT likewise) */
    if (!abort_poll && ReadBlock(dev, MB_FC_READ_HOLDING, GW_REG_VPV, 3, b, &abort_poll))
    {
        d.vpv = (float)Modbus_U16(b, 0) / 10.0f;
        d.ipv = (float)Modbus_U16(b, (uint16_t)(GW_REG_IPV - GW_REG_VPV)) / 10.0f;
        d.ok_pv = true;
    }

    /* inverter active power */
    if (!abort_poll)
    {
#if RESS_GOODWE_MT
        if (ReadBlock(dev, MB_FC_READ_HOLDING, GW_REG_POUT, 2, b, &abort_poll))
        {
            d.pout = (float)U32_BE(b, 0);
            d.ok_pout = true;
        }
#else
        if (ReadBlock(dev, MB_FC_READ_HOLDING, GW_REG_POUT, 1, b, &abort_poll))
        {
            d.pout = (float)Modbus_U16(b, 0);
            d.ok_pout = true;
        }
#endif
    }

    /* work mode + temperature are adjacent on both maps */
    if (!abort_poll)
    {
        uint16_t first = (GW_REG_STATUS < GW_REG_TEMP) ? GW_REG_STATUS : GW_REG_TEMP;
        uint16_t last  = (GW_REG_STATUS > GW_REG_TEMP) ? GW_REG_STATUS : GW_REG_TEMP;
        uint8_t  sb[8];

        if (ReadBlock(dev, MB_FC_READ_HOLDING, first, (uint16_t)(last - first + 1U), sb, &abort_poll))
        {
            d.status = GoodWeStatus(Modbus_U16(sb, (uint16_t)(GW_REG_STATUS - first)));
            d.temp = (float)Modbus_U16(sb, (uint16_t)(GW_REG_TEMP - first)) / 10.0f;
            d.ok_status = true;
            d.ok_temp = true;
        }
    }

    /* PV power: Vpv * Ipv (Non-MT) or inverter output (MT), per the plan */
    if (d.ok_pv)
    {
#if RESS_GOODWE_MT
        if (d.ok_pout)
        {
            d.ppv = d.pout;
            d.ok_ppv = true;
        }
#else
        d.ppv = d.vpv * d.ipv;
        d.ok_ppv = true;
#endif
    }

    /* no external meter wired to the inverter: load power is 0 */
    d.pload = 0.0f;
    d.ok_load = true;

    bool any = d.ok_energy || d.ok_pv || d.ok_pout || d.ok_status;
    if (any)
    {
        d.online = true;
        d.update_tick = HAL_GetTick();
        s_gw = d;

        char v[12], i[12], p[12], e[14], t[12];
        RESS_FmtFixed(v, sizeof(v), d.vpv, 1);
        RESS_FmtFixed(i, sizeof(i), d.ipv, 1);
        RESS_FmtFixed(p, sizeof(p), d.pout, 0);
        RESS_FmtFixed(e, sizeof(e), d.energy_kwh, 1);
        RESS_FmtFixed(t, sizeof(t), d.temp, 1);
        RESS_LOG("GOODWE", "Vpv=%s V Ipv=%s A Pout=%s W E=%s kWh T=%s C status=%s", v, i, p, e, t,
                 d.status);
    }
    return any;
}

void GoodWe_Init(uint8_t slave_id, uint32_t poll_interval_ms)
{
    memset(&s_gw, 0, sizeof(s_gw));
    s_gw.status = "unknown";

    s_gw_dev.name = "GOODWE";
    s_gw_dev.slave_id = slave_id;
    s_gw_dev.dev_type = DEVICE_TYPE_INVERTER;
    s_gw_dev.format = MB_FMT_8N1;
    s_gw_dev.poll_interval_ms = poll_interval_ms;
    s_gw_dev.offline_retry_ms = RESS_INVERTER_OFFLINE_RETRY_MS;
    s_gw_dev.poll = PollGoodWe;

    RESS_LOG("GOODWE", "init: slave=%u register map=%s", slave_id, RESS_GOODWE_MT ? "MT/SMT" : "Non-MT/SDTG2");
    (void)Modbus_RegisterDevice(&s_gw_dev);
}

void GoodWe_GetLatest(Inverter_Data_t *out)
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
    s_sg_dev.poll = PollSungrow;

    RESS_LOG("SUNGROW", "init: slave=%u (FC 0x04)", slave_id);
    (void)Modbus_RegisterDevice(&s_sg_dev);
}

void Sungrow_GetLatest(Inverter_Data_t *out)
{
    *out = s_sg;
    out->online = s_sg_dev.is_online && s_sg.update_tick != 0U;
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
