#ifndef W5500_NETCFG_H
#define W5500_NETCFG_H

#include "wizchip_conf.h"

/* LAN (W5500): local/field network, static addressing. 192.168.1.0/24
 * is chosen to avoid colliding with the typical 192.168.0.x/192.168.10.x+
 * ranges a site router hands out. Installer can override at commissioning. */
static const wiz_NetInfo W5500_LAN_NET_INFO = {
    .mac  = {0x02, 0x00, 0x00, 0x52, 0x45, 0x31}, /* locally-administered, "RE1" */
    .ip   = {192, 168, 1, 50},
    .sn   = {255, 255, 255, 0},
    .gw   = {192, 168, 1, 1},
    .dns  = {0, 0, 0, 0},
    .dhcp = NETINFO_STATIC
};

#endif /* W5500_NETCFG_H */
