#ifndef CLIENT_UTILS_H
#define CLIENT_UTILS_H

#include "client_common.h"

void parse_json_string(const char *json, const char *key, char *value, int max_len);
int parse_json_int(const char *json, const char *key);

#endif /* CLIENT_UTILS_H */
