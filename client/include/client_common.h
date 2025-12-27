#ifndef CLIENT_COMMON_H
#define CLIENT_COMMON_H

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NM_PORT 9000
#define BUFFER_SIZE 8192
#define MAX_USERNAME 64
#define MAX_FILENAME 256

extern char NM_IP[INET_ADDRSTRLEN];
extern char current_username[MAX_USERNAME];

#endif /* CLIENT_COMMON_H */
