#ifndef SS_UTILS_H
#define SS_UTILS_H

#include "ss_common.h"

// JSON utilities
int json_get_string(const char *json, const char *key, char *out, size_t out_size);
int json_get_int(const char *json, const char *key, int *out);
char *json_escape(const char *src);

// Network utilities
int is_loopback_address(const char *ip);
int choose_non_loopback_ipv4(char *out, size_t out_size);

// Logging
void init_logging(void);
void log_event(const char *level, const char *ip, int port, const char *username, 
               const char *cmd, const char *payload);

#endif // SS_UTILS_H
