#ifndef NM_NETWORK_H
#define NM_NETWORK_H

#include "nm_common.h"

void *handle_connection(void *arg);
void send_response(int fd, const char *response);
char *read_request(int fd);

#endif /* NM_NETWORK_H */
