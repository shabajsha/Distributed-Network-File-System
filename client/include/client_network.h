#ifndef CLIENT_NETWORK_H
#define CLIENT_NETWORK_H

#include "client_common.h"

int connect_to_nm(void);
int connect_to_ss(const char *ip, int port);
void send_message(int fd, const char *message);
char *receive_message(int fd);

#endif /* CLIENT_NETWORK_H */
