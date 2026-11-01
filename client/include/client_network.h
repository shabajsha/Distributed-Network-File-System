#ifndef CLIENT_NETWORK_H
#define CLIENT_NETWORK_H

#include "client_common.h"

/* One TCP connection plus a buffer for reading newline-terminated replies. */
typedef struct {
    int fd;
    LineReader reader;
} Conn;

int conn_open(Conn *conn, const char *ip, int port);
int conn_send(Conn *conn, const char *message);
char *conn_recv(Conn *conn);
void conn_close(Conn *conn);

int login(void);
char *nm_call(const char *cmd, const char *fields);

#endif /* CLIENT_NETWORK_H */
