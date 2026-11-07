#include "ress_util.h"
#include <stdbool.h>

int RESS_FmtFixed(char *buf, size_t size, float value, uint8_t decimals)
{
    static const uint32_t p10[] = { 1U, 10U, 100U, 1000U, 10000U };

    if (decimals > 4U)
    {
        decimals = 4U;
    }

    if (value != value) /* NaN guard */
    {
        value = 0.0f;
    }

    bool neg = (value < 0.0f);
    double a = neg ? -(double)value : (double)value;
    if (a > 4.0e9 / 10000.0)
    {
        a = 4.0e9 / 10000.0; /* keep the scaled value inside uint32 */
    }

    uint32_t scaled = (uint32_t)(a * (double)p10[decimals] + 0.5);
    uint32_t ip = scaled / p10[decimals];
    uint32_t fp = scaled % p10[decimals];

    if (scaled == 0U)
    {
        neg = false; /* avoid "-0.0" */
    }

    if (decimals == 0U)
    {
        return snprintf(buf, size, "%s%lu", neg ? "-" : "", (unsigned long)ip);
    }

    return snprintf(buf, size, "%s%lu.%0*lu", neg ? "-" : "", (unsigned long)ip,
                    (int)decimals, (unsigned long)fp);
}
