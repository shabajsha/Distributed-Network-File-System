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

#include "dnfs_shared.h"

#define MAX_USERNAME 64
#define MAX_FILENAME 256
#define MAX_PASSWORD 256

extern char NM_IP[INET_ADDRSTRLEN];
extern char current_username[MAX_USERNAME];
extern char current_password[MAX_PASSWORD];
extern char session_token[DNFS_HEX_LEN + 1];

#endif /* CLIENT_COMMON_H */
