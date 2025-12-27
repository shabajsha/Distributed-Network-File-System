#ifndef SS_NETWORK_H
#define SS_NETWORK_H

#include "ss_common.h"

// Network operations
void register_with_nm(void);
void handle_client(int client_sock);
void *client_thread(void *arg);

// Client thread argument
typedef struct {
    int socket_fd;
    struct sockaddr_in addr;
} ClientThreadArg;

#endif // SS_NETWORK_H
