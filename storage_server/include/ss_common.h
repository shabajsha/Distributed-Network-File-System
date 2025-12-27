#ifndef SS_COMMON_H
#define SS_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <time.h>

// Constants
#define NM_PORT 9000
#define MAX_FILENAME 256
#define MAX_MSG 4096
#define MAX_LOCKS 128
#define MAX_USERNAME 64

// Global path variables
extern char BASE_DIR[1024];
extern char DATA_DIR[1024];
extern char SNAP_DIR[1024];
extern char LOG_DIR[1024];
extern char NM_IP[INET_ADDRSTRLEN];
extern char ADVERTISE_IP[INET_ADDRSTRLEN];
extern int CLIENT_PORT;

// Logging context (thread-local)
typedef struct {
    char ip[INET_ADDRSTRLEN];
    int port;
    char username[MAX_USERNAME];
    char cmd[32];
} ClientLogContext;

extern __thread ClientLogContext g_log_ctx;

#endif // SS_COMMON_H
