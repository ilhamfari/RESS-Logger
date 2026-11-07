#include "w5500_app.h"
#include "w5500_port.h"
#include "socket.h"
#include <stdio.h>
#include <string.h>

void LAN_App_Init(void)
{
    socket(LAN_HTTP_SOCKET, Sn_MR_TCP, LAN_HTTP_PORT, 0);
    listen(LAN_HTTP_SOCKET);
}

void LAN_Webserver_Poll(void)
{
    uint8_t status = getSn_SR(LAN_HTTP_SOCKET);

    switch (status)
    {
    case SOCK_ESTABLISHED:
        if (getSn_RX_RSR(LAN_HTTP_SOCKET) > 0)
        {
            uint8_t rxbuf[64];
            int32_t len = recv(LAN_HTTP_SOCKET, rxbuf, sizeof(rxbuf) - 1);
            if (len > 0)
            {
                static const char reply[] =
                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
                send(LAN_HTTP_SOCKET, (uint8_t *)reply, sizeof(reply) - 1);
                disconnect(LAN_HTTP_SOCKET);
            }
        }
        break;

    case SOCK_CLOSE_WAIT:
        disconnect(LAN_HTTP_SOCKET);
        break;

    case SOCK_CLOSED:
        socket(LAN_HTTP_SOCKET, Sn_MR_TCP, LAN_HTTP_PORT, 0);
        listen(LAN_HTTP_SOCKET);
        break;

    default:
        break;
    }
}
