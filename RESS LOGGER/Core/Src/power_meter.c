#include "power_meter.h"
#include "ress_util.h"
#include <string.h>
#include <stdlib.h>

/* Register offsets relative to PM_START_REG (0x0023). */
enum
{
    R_DPT_DCT = 0,
    R_DPQ_SIGN,
    R_UA, R_UB, R_UC, R_UAB, R_UBC, R_UCA,
    R_IA, R_IB, R_IC,
    R_PA, R_PB, R_PC, R_PS,
    R_QA, R_QB, R_QC, R_QS,
    R_PF1, R_PF2, R_PF3, R_PFS,
    R_SA, R_SB, R_SC, R_SS,
    R_FREQ
};

/* SIGN byte bit order: 1 = positive, 0 = negative. */
enum
{
    SIGN_PA = 0, SIGN_PB, SIGN_PC, SIGN_PS,
    SIGN_QA, SIGN_QB, SIGN_QC, SIGN_QS
};

#define PM_MAX_DECIMAL_EXP  6U

static ModbusDevice_t    s_dev;
static PowerMeter_Data_t s_latest;
static uint32_t          s_decode_count;

static uint16_t Rd16(const uint8_t *p, uint8_t idx)
{
    return (uint16_t)(((uint16_t)p[2U * idx] << 8) | p[2U * idx + 1U]);
}

static double Pow10u(uint8_t e)
{
    static const double t[] = { 1.0, 10.0, 100.0, 1e3, 1e4, 1e5, 1e6 };
    return t[e];
}

/* Rx / 10000 * 10^exp, magnitude only (sign handled separately). */
static float ScaleUnsigned(uint16_t raw, uint8_t exp10)
{
    int16_t s = (int16_t)raw;
    uint16_t mag = (uint16_t)(s < 0 ? -s : s);
    return (float)((double)mag / 10000.0 * Pow10u(exp10));
}

static float ScalePower(uint16_t raw, uint8_t dpq, uint8_t sign, uint8_t bit)
{
    float v = ScaleUnsigned(raw, dpq);
    return (((sign >> bit) & 1U) != 0U) ? v : -v;
}

bool PowerMeter_Decode(const uint8_t *data, uint16_t len, PowerMeter_Data_t *out)
{
    if (data == NULL || out == NULL || len < (2U * PM_REG_COUNT))
    {
        return false;
    }

    uint16_t r0 = Rd16(data, R_DPT_DCT);
    uint16_t r1 = Rd16(data, R_DPQ_SIGN);
    uint8_t dpt = (uint8_t)(r0 >> 8);
    uint8_t dct = (uint8_t)(r0 & 0xFFU);
    uint8_t dpq = (uint8_t)(r1 >> 8);
    uint8_t sgn = (uint8_t)(r1 & 0xFFU);

    if (dpt > PM_MAX_DECIMAL_EXP || dct > PM_MAX_DECIMAL_EXP || dpq > PM_MAX_DECIMAL_EXP)
    {
        RESS_LOG("METER", "scaling out of range: DPT=%u DCT=%u DPQ=%u", dpt, dct, dpq);
        return false;
    }

    PowerMeter_Data_t d;
    memset(&d, 0, sizeof(d));
    d.dpt = dpt;
    d.dct = dct;
    d.dpq = dpq;
    d.sign = sgn;

    d.ua  = ScaleUnsigned(Rd16(data, R_UA),  dpt);
    d.ub  = ScaleUnsigned(Rd16(data, R_UB),  dpt);
    d.uc  = ScaleUnsigned(Rd16(data, R_UC),  dpt);
    d.uab = ScaleUnsigned(Rd16(data, R_UAB), dpt);
    d.ubc = ScaleUnsigned(Rd16(data, R_UBC), dpt);
    d.uca = ScaleUnsigned(Rd16(data, R_UCA), dpt);

    d.ia = ScaleUnsigned(Rd16(data, R_IA), dct);
    d.ib = ScaleUnsigned(Rd16(data, R_IB), dct);
    d.ic = ScaleUnsigned(Rd16(data, R_IC), dct);

    d.pa      = ScalePower(Rd16(data, R_PA), dpq, sgn, SIGN_PA);
    d.pb      = ScalePower(Rd16(data, R_PB), dpq, sgn, SIGN_PB);
    d.pc      = ScalePower(Rd16(data, R_PC), dpq, sgn, SIGN_PC);
    d.p_total = ScalePower(Rd16(data, R_PS), dpq, sgn, SIGN_PS);

    d.qa      = ScalePower(Rd16(data, R_QA), dpq, sgn, SIGN_QA);
    d.qb      = ScalePower(Rd16(data, R_QB), dpq, sgn, SIGN_QB);
    d.qc      = ScalePower(Rd16(data, R_QC), dpq, sgn, SIGN_QC);
    d.q_total = ScalePower(Rd16(data, R_QS), dpq, sgn, SIGN_QS);

    d.sa      = ScaleUnsigned(Rd16(data, R_SA), dpq);
    d.sb      = ScaleUnsigned(Rd16(data, R_SB), dpq);
    d.sc      = ScaleUnsigned(Rd16(data, R_SC), dpq);
    d.s_total = ScaleUnsigned(Rd16(data, R_SS), dpq);

    d.pf1    = (float)((int16_t)Rd16(data, R_PF1) / 1000.0);
    d.pf2    = (float)((int16_t)Rd16(data, R_PF2) / 1000.0);
    d.pf3    = (float)((int16_t)Rd16(data, R_PF3) / 1000.0);
    d.pf_avg = (float)((int16_t)Rd16(data, R_PFS) / 1000.0);

    d.freq = (float)((int16_t)Rd16(data, R_FREQ) / 100.0);

    d.update_tick = HAL_GetTick();
    d.valid = true;

    *out = d;
    return true;
}

static bool OnData(const ModbusDevice_t *dev, const uint8_t *data, uint16_t len)
{
    (void)dev;
    PowerMeter_Data_t d;

    if (!PowerMeter_Decode(data, len, &d))
    {
        return false;
    }

    s_latest = d;
    s_decode_count++;

    char v[12], i[12], p[12], pf[12], f[12];
    RESS_FmtFixed(v, sizeof(v), d.ua, 1);
    RESS_FmtFixed(i, sizeof(i), d.ia, 3);
    RESS_FmtFixed(p, sizeof(p), d.p_total, 1);
    RESS_FmtFixed(pf, sizeof(pf), d.pf_avg, 3);
    RESS_FmtFixed(f, sizeof(f), d.freq, 2);
    RESS_LOG("METER", "#%lu Ua=%s V Ia=%s A P=%s W PF=%s F=%s Hz (DPT=%u DCT=%u DPQ=%u SIGN=0x%02X)",
             (unsigned long)s_decode_count, v, i, p, pf, f, d.dpt, d.dct, d.dpq, d.sign);
    return true;
}

void PowerMeter_Init(uint8_t slave_id, uint32_t poll_interval_ms)
{
    memset(&s_latest, 0, sizeof(s_latest));
    s_decode_count = 0;

    s_dev.name = "PowerMeter";
    s_dev.slave_id = slave_id;
    s_dev.dev_type = DEVICE_TYPE_POWER_METER;
    s_dev.start_reg = PM_START_REG;
    s_dev.reg_count = PM_REG_COUNT;
    s_dev.poll_interval_ms = poll_interval_ms;
    s_dev.parse = OnData;

    RESS_LOG("METER", "init: slave=0x%02X, block 0x%04X + %u regs", slave_id,
             (unsigned)PM_START_REG, (unsigned)PM_REG_COUNT);
    (void)Modbus_RegisterDevice(&s_dev);
}

bool PowerMeter_IsOnline(void)
{
    return s_dev.is_online && s_latest.valid;
}

bool PowerMeter_GetLatest(PowerMeter_Data_t *out)
{
    if (out != NULL)
    {
        *out = s_latest;
    }
    return PowerMeter_IsOnline();
}

int PowerMeter_FormatFields(char *buf, size_t size, const PowerMeter_Data_t *d)
{
    typedef struct { float v; uint8_t dec; } Field_t;
    const Field_t f[] =
    {
        { d->ua, 1 },  { d->ub, 1 },  { d->uc, 1 },
        { d->uab, 1 }, { d->ubc, 1 }, { d->uca, 1 },
        { d->ia, 3 },  { d->ib, 3 },  { d->ic, 3 },
        { d->pa, 1 },  { d->pb, 1 },  { d->pc, 1 },  { d->p_total, 1 },
        { d->qa, 1 },  { d->qb, 1 },  { d->qc, 1 },  { d->q_total, 1 },
        { d->sa, 1 },  { d->sb, 1 },  { d->sc, 1 },  { d->s_total, 1 },
        { d->pf1, 3 }, { d->pf2, 3 }, { d->pf3, 3 }, { d->pf_avg, 3 },
        { d->freq, 2 }
    };

    size_t pos = 0;
    for (size_t k = 0; k < (sizeof(f) / sizeof(f[0])); k++)
    {
        if (pos + 2U >= size)
        {
            break;
        }
        if (k > 0U)
        {
            buf[pos++] = ',';
        }
        int n = RESS_FmtFixed(&buf[pos], size - pos, f[k].v, f[k].dec);
        if (n < 0)
        {
            break;
        }
        pos += (size_t)n;
    }
    if (pos < size)
    {
        buf[pos] = '\0';
    }
    return (int)pos;
}

void PowerMeter_PrintDebug(const PowerMeter_Data_t *d)
{
    if (!d->valid)
    {
        RESS_LOG("METER", "no valid data yet (online=%d)", (int)s_dev.is_online);
        return;
    }

    char b[200];
    PowerMeter_FormatFields(b, sizeof(b), d);
    RESS_LOG("METER", "online=%d ok=%lu err=%lu age=%lu ms",
             (int)s_dev.is_online, (unsigned long)s_dev.ok_count,
             (unsigned long)s_dev.err_count, (unsigned long)(HAL_GetTick() - d->update_tick));
    RESS_LOG("METER", "ua,ub,uc,uab,ubc,uca,ia,ib,ic,pa,pb,pc,p,qa,qb,qc,q,sa,sb,sc,s,pf1,pf2,pf3,pfavg,f");
    RESS_LOG("METER", "%s", b);
}
