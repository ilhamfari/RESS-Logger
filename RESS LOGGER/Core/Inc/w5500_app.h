#ifndef W5500_APP_H
#define W5500_APP_H

#include <stdint.h>

#define WAN_DHCP_SOCKET   0U   /* socket 0 reserved for DHCP on the WAN chip */
#define LAN_HTTP_SOCKET   0U   /* socket 0 reserved for the stub listener on the LAN chip */
#define LAN_HTTP_PORT     80U

/* Called once after W5500_HardInit(W5500_WAN, ...) and DHCP_init(). */
void WAN_App_Init(void);

/* Called once after W5500_HardInit(W5500_LAN, ...). Opens the v1 stub
 * listen socket. */
void LAN_App_Init(void);

/* Non-blocking, one iteration per superloop pass. Caller must have already
 * called W5500_Select(W5500_WAN). Services DHCP and reports link/lease
 * status; deferred: MQTT/HTTP cloud push, NTP. */
void WAN_Service_Poll(void);

/* Non-blocking, one iteration per superloop pass. Caller must have already
 * called W5500_Select(W5500_LAN). v1: accepts a TCP connection on port 80
 * and replies "OK" to prove the LAN link/static IP path works; deferred:
 * real HTTP parsing/content, auth, Modbus TCP. */
void LAN_Webserver_Poll(void);

#endif /* W5500_APP_H */
