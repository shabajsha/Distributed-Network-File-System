#include "dnfs_shared.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Names may only use letters, digits, '.', '_' and '-', and may not start
 * with '.'. That rules out path traversal ("../x", "a/b"), hidden temp files
 * and anything that would need escaping. */
static int valid_name(const char *name, size_t max_len) {
    if (!name || !*name) {
        return 0;
    }
    size_t len = strlen(name);
    if (len > max_len || name[0] == '.') {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-') {
            return 0;
        }
    }
    return 1;
}

int dnfs_valid_filename(const char *name) {
    return valid_name(name, 255);
}

int dnfs_valid_username(const char *name) {
    return valid_name(name, 63);
}

int dnfs_env_int(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (*end != '\0' || parsed <= 0 || parsed > 65535 * 16) {
        return fallback;
    }
    return (int)parsed;
}

int dnfs_nm_port(void) {
    return dnfs_env_int("DNFS_NM_PORT", DNFS_DEFAULT_NM_PORT);
}
