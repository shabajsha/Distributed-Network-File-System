#ifndef NM_NETWORK_H
#define NM_NETWORK_H

#include "nm_common.h"

void *handle_connection(void *arg);
void send_response(int fd, const char *response);
void send_error(int fd, const char *reason);
char *read_request(int fd);
char *ss_call(const char *ip, int port, const char *request, int timeout_sec);

#endif /* NM_NETWORK_H */
