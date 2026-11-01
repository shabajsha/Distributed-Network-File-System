//AI code starts
#include "client_utils.h"

void parse_json_string(const char *json, const char *key, char *value, int max_len) {
    if (!value || max_len <= 0) {
        return;
    }
    if (!json || !key || !json_get_str(json, key, value, (size_t)max_len)) {
        value[0] = '\0';
    }
}

int parse_json_int(const char *json, const char *key) {
    long value = 0;
    if (!json || !key || !json_get_long(json, key, &value)) {
        return 0;
    }
    return (int)value;
}

int response_is_ok(const char *response) {
    char status[16] = {0};
    return response && json_get_str(response, "status", status, sizeof(status)) && strcmp(status, "OK") == 0;
}

/* Prints "<prefix>: <reason>" for an error reply. */
void print_error(const char *response, const char *prefix) {
    char reason[128] = {0};
    parse_json_string(response, "reason", reason, sizeof(reason));
    printf("%s: %s\n", prefix, reason[0] ? reason : "UNKNOWN");
}
//AI code ends
