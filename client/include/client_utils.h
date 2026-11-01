#ifndef CLIENT_UTILS_H
#define CLIENT_UTILS_H

#include "client_common.h"

void parse_json_string(const char *json, const char *key, char *value, int max_len);
int parse_json_int(const char *json, const char *key);
int response_is_ok(const char *response);
void print_error(const char *response, const char *prefix);

#endif /* CLIENT_UTILS_H */
