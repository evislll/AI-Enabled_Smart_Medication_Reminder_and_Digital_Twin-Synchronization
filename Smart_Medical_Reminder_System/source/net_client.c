#include "net_client.h"

#include <stdio.h>
#include <string.h>

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "fsl_debug_console.h"


///////////////////////////////////////////////////////////////////////////////////
//--------------------------Whole file created using ChatGpt---------------------//
//--------------------------but figured out lot of stuff manually----------------//
//----------------like how the data should be written using json-----------------//
///////////////////////////////////////////////////////////////////////////////////



static char g_server_ip[32] = {0};
static unsigned short g_server_port = 0;

bool NetClient_Init(const char *server_ip, unsigned short server_port)
{
    if ((server_ip == NULL) || (strlen(server_ip) >= sizeof(g_server_ip)))
    {
        PRINTF("[NET CLIENT] Invalid server IP\r\n");
        return false;
    }

    strcpy(g_server_ip, server_ip);
    g_server_port = server_port;

    PRINTF("[NET CLIENT] Server configured: %s:%u\r\n", g_server_ip, g_server_port);
    return true;
}

bool NetClient_PostJson(const char *path, const char *json)
{
    int sock;
    struct sockaddr_in server_addr;
    char request[768];
    int json_len;
    int request_len;
    int total_sent;
    int sent_now;

    if ((path == NULL) || (json == NULL))
    {
        PRINTF("[NET CLIENT] path/json is NULL\r\n");
        return false;
    }

    if ((g_server_ip[0] == '\0') || (g_server_port == 0))
    {
        PRINTF("[NET CLIENT] Server not initialized\r\n");
        return false;
    }

    json_len = (int)strlen(json);

    request_len = snprintf(
        request,
        sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path,
        g_server_ip,
        g_server_port,
        json_len,
        json
    );

    if ((request_len <= 0) || (request_len >= (int)sizeof(request)))
    {
        PRINTF("[NET CLIENT] HTTP request build failed\r\n");
        return false;
    }

    sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        PRINTF("[NET CLIENT] Socket create failed\r\n");
        return false;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(g_server_port);
    server_addr.sin_addr.s_addr = inet_addr(g_server_ip);

    if (lwip_connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        PRINTF("[NET CLIENT] Connect failed\r\n");
        lwip_close(sock);
        return false;
    }

    total_sent = 0;
    while (total_sent < request_len)
    {
        sent_now = lwip_send(sock,
                             request + total_sent,
                             request_len - total_sent,
                             0);

        if (sent_now <= 0)
        {
            PRINTF("[NET CLIENT] HTTP send failed\r\n");
            lwip_close(sock);
            return false;
        }

        total_sent += sent_now;
    }

    lwip_shutdown(sock, SHUT_WR);
    lwip_close(sock);
    return true;
}

bool NetClient_GetCommand(const char *path, char *out_cmd, size_t out_cmd_size)
{
    int sock;
    struct sockaddr_in server_addr;
    char request[256];
    char response[512];
    char *body;
    int request_len;
    int total_received;
    int recv_len;

    if ((path == NULL) || (out_cmd == NULL) || (out_cmd_size == 0U))
    {
        PRINTF("[NET CLIENT] GetCommand invalid args\r\n");
        return false;
    }

    if ((g_server_ip[0] == '\0') || (g_server_port == 0U))
    {
        PRINTF("[NET CLIENT] Server not initialized\r\n");
        return false;
    }

    out_cmd[0] = '\0';

    request_len = snprintf(
        request,
        sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Connection: close\r\n"
        "\r\n",
        path,
        g_server_ip,
        g_server_port
    );

    if ((request_len <= 0) || (request_len >= (int)sizeof(request)))
    {
        PRINTF("[NET CLIENT] GET request build failed\r\n");
        return false;
    }

    sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        PRINTF("[NET CLIENT] Socket create failed\r\n");
        return false;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(g_server_port);
    server_addr.sin_addr.s_addr = inet_addr(g_server_ip);

    if (lwip_connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        PRINTF("[NET CLIENT] GET connect failed\r\n");
        lwip_close(sock);
        return false;
    }

    if (lwip_send(sock, request, request_len, 0) <= 0)
    {
        PRINTF("[NET CLIENT] GET send failed\r\n");
        lwip_close(sock);
        return false;
    }

    total_received = 0;
    memset(response, 0, sizeof(response));

    while (total_received < ((int)sizeof(response) - 1))
    {
        recv_len = lwip_recv(sock,
                             &response[total_received],
                             sizeof(response) - 1 - total_received,
                             0);

        if (recv_len <= 0)
        {
            break;
        }

        total_received += recv_len;
    }

    lwip_shutdown(sock, SHUT_RDWR);
    lwip_close(sock);

    if (total_received <= 0)
    {
        PRINTF("[NET CLIENT] GET recv failed\r\n");
        return false;
    }

    response[total_received] = '\0';

    body = strstr(response, "\r\n\r\n");
    if (body == NULL)
    {
        PRINTF("[NET CLIENT] Invalid HTTP response\r\n");
        return false;
    }

    body += 4;

    strncpy(out_cmd, body, out_cmd_size - 1);
    out_cmd[out_cmd_size - 1] = '\0';

    {
        size_t len = strlen(out_cmd);
        while ((len > 0U) &&
               ((out_cmd[len - 1] == '\r') ||
                (out_cmd[len - 1] == '\n') ||
                (out_cmd[len - 1] == ' ')))
        {
            out_cmd[len - 1] = '\0';
            len--;
        }
    }

    return true;
}



bool NetClient_GetText(const char *path, char *out_text, size_t out_text_size)
{
    int sock;
    struct sockaddr_in server_addr;
    char request[256];
    char response[768];
    char *body;
    int request_len;
    int total_received;
    int recv_len;
    size_t body_len;

    if ((path == NULL) || (out_text == NULL) || (out_text_size == 0U))
    {
        PRINTF("[NET CLIENT] GetText invalid args\r\n");
        return false;
    }

    if ((g_server_ip[0] == '\0') || (g_server_port == 0U))
    {
        PRINTF("[NET CLIENT] Server not initialized\r\n");
        return false;
    }

    out_text[0] = '\0';

    request_len = snprintf(
        request,
        sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Connection: close\r\n"
        "\r\n",
        path,
        g_server_ip,
        g_server_port
    );

    if ((request_len <= 0) || (request_len >= (int)sizeof(request)))
    {
        PRINTF("[NET CLIENT] GET request build failed\r\n");
        return false;
    }

    sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        PRINTF("[NET CLIENT] Socket create failed\r\n");
        return false;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(g_server_port);
    server_addr.sin_addr.s_addr = inet_addr(g_server_ip);

    if (lwip_connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        PRINTF("[NET CLIENT] GET connect failed\r\n");
        lwip_close(sock);
        return false;
    }

    if (lwip_send(sock, request, request_len, 0) <= 0)
    {
        PRINTF("[NET CLIENT] GET send failed\r\n");
        lwip_close(sock);
        return false;
    }

    total_received = 0;
    memset(response, 0, sizeof(response));

    while (total_received < ((int)sizeof(response) - 1))
    {
        recv_len = lwip_recv(sock,
                             &response[total_received],
                             sizeof(response) - 1 - total_received,
                             0);

        if (recv_len <= 0)
        {
            break;
        }

        total_received += recv_len;
    }

    lwip_shutdown(sock, SHUT_RDWR);
    lwip_close(sock);

    if (total_received <= 0)
    {
        PRINTF("[NET CLIENT] GET recv failed\r\n");
        return false;
    }

    response[total_received] = '\0';

    body = strstr(response, "\r\n\r\n");
    if (body == NULL)
    {
        PRINTF("[NET CLIENT] Invalid HTTP response\r\n");
        return false;
    }

    body += 4;
    body_len = strlen(body);

    if (body_len >= out_text_size)
    {
        PRINTF("[NET CLIENT] Body too large\r\n");
        return false;
    }

    memcpy(out_text, body, body_len + 1U);
    return true;
}






