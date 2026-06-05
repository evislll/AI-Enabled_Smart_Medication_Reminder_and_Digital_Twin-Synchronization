#ifndef NET_CLIENT_H_
#define NET_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

bool NetClient_Init(const char *server_ip, unsigned short server_port);
bool NetClient_PostJson(const char *path, const char *json);
bool NetClient_GetCommand(const char *path, char *out_cmd, size_t out_cmd_size);
bool NetClient_GetText(const char *path, char *out_text, size_t out_text_size);


#ifdef __cplusplus
}
#endif

#endif /* NET_CLIENT_H_ */
