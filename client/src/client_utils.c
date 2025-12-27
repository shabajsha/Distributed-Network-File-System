//AI code starts
#include "client_utils.h"

void parse_json_string(const char *json, const char *key, char *value, int max_len) {
    if (!json || !key || !value || max_len <= 0) {
        return;
    }

    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *pos = strstr(json, search_key);
    if (!pos) {
        return;
    }

    pos = strchr(pos, ':');
    if (!pos) {
        return;
    }
    pos++;

    while (*pos == ' ' || *pos == '\t') {
        pos++;
    }

    if (*pos == '"') {
        pos++;
        const char *end = pos;
        while (*end) {
            if (*end == '\\' && *(end + 1)) {
                end += 2;
                continue;
            }
            if (*end == '"') {
                break;
            }
            end++;
        }

        if (*end == '"') {
            int len = (int)(end - pos);
            if (len >= max_len) {
                len = max_len - 1;
            }

            int idx = 0;
            while (pos < end && idx < len) {
                if (*pos == '\\' && (pos + 1) < end) {
                    pos++;
                    switch (*pos) {
                        case 'n': value[idx++] = '\n'; break;
                        case 'r': value[idx++] = '\r'; break;
                        case 't': value[idx++] = '\t'; break;
                        case '\\': value[idx++] = '\\'; break;
                        case '"': value[idx++] = '"'; break;
                        case '/': value[idx++] = '/'; break;
                        default: value[idx++] = *pos; break;
                    }
                    pos++;
                } else {
                    value[idx++] = *pos++;
                }
            }
            value[idx] = '\0';
        }
    }
}

int parse_json_int(const char *json, const char *key) {
    if (!json || !key) {
        return 0;
    }

    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *pos = strstr(json, search_key);
    if (!pos) {
        return 0;
    }

    pos = strchr(pos, ':');
    if (!pos) {
        return 0;
    }
    pos++;

    while (*pos == ' ' || *pos == '\t') {
        pos++;
    }

    return atoi(pos);
}
//AI code ends