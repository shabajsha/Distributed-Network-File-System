#include "dnfs_shared.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *json_escape(const char *src) {
    if (!src) {
        src = "";
    }
    size_t extra = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (*p == '\\' || *p == '"' || *p == '\n' || *p == '\r' || *p == '\t') {
            extra += 1;
        } else if (*p < 0x20) {
            extra += 5;
        }
    }

    size_t len = strlen(src);
    char *out = malloc(len + extra + 1);
    if (!out) {
        return NULL;
    }

    char *w = out;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        switch (*p) {
            case '\\': *w++ = '\\'; *w++ = '\\'; break;
            case '"':  *w++ = '\\'; *w++ = '"'; break;
            case '\n': *w++ = '\\'; *w++ = 'n'; break;
            case '\r': *w++ = '\\'; *w++ = 'r'; break;
            case '\t': *w++ = '\\'; *w++ = 't'; break;
            default:
                if (*p < 0x20) {
                    snprintf(w, 7, "\\u%04x", *p);
                    w += 6;
                } else {
                    *w++ = (char)*p;
                }
                break;
        }
    }
    *w = '\0';
    return out;
}

const char *json_skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *skip_string(const char *p) {
    if (*p != '"') {
        return NULL;
    }
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (!*p) {
                return NULL;
            }
        }
        p++;
    }
    return *p == '"' ? p + 1 : NULL;
}

/* Returns the position just after the JSON value starting at p, or NULL. */
const char *json_skip_value(const char *p) {
    p = json_skip_ws(p);
    if (!p || !*p) {
        return NULL;
    }
    if (*p == '"') {
        return skip_string(p);
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = skip_string(p);
                if (!p) {
                    return NULL;
                }
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return NULL;
    }
    const char *start = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) {
        p++;
    }
    return p > start ? p : NULL;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes the JSON string token at p (which must point at '"'). */
char *json_read_string(const char *p, const char **end) {
    const char *stop = skip_string(p);
    if (!stop) {
        return NULL;
    }
    char *out = malloc((size_t)(stop - p));
    if (!out) {
        return NULL;
    }

    char *w = out;
    p++;
    while (p < stop - 1) {
        if (*p != '\\') {
            *w++ = *p++;
            continue;
        }
        p++;
        switch (*p) {
            case 'n': *w++ = '\n'; p++; break;
            case 'r': *w++ = '\r'; p++; break;
            case 't': *w++ = '\t'; p++; break;
            case 'b': *w++ = '\b'; p++; break;
            case 'f': *w++ = '\f'; p++; break;
            case 'u': {
                unsigned int code = 0;
                int ok = 1;
                for (int i = 1; i <= 4; i++) {
                    int h = hex_value(p[i]);
                    if (h < 0) {
                        ok = 0;
                        break;
                    }
                    code = code * 16 + (unsigned int)h;
                }
                if (!ok) {
                    *w++ = 'u';
                    p++;
                    break;
                }
                if (code < 0x80) {
                    *w++ = (char)code;
                } else if (code < 0x800) {
                    *w++ = (char)(0xC0 | (code >> 6));
                    *w++ = (char)(0x80 | (code & 0x3F));
                } else {
                    *w++ = (char)(0xE0 | (code >> 12));
                    *w++ = (char)(0x80 | ((code >> 6) & 0x3F));
                    *w++ = (char)(0x80 | (code & 0x3F));
                }
                p += 5;
                break;
            }
            default: *w++ = *p++; break;
        }
    }
    *w = '\0';
    if (end) {
        *end = stop;
    }
    return out;
}

/* Walks the members of the top-level object, so a key name that happens to
 * appear inside a value or a nested object is never matched. */
const char *json_find_value(const char *json, const char *key) {
    const char *p = json_skip_ws(json);
    if (!p || *p != '{') {
        return NULL;
    }
    p++;
    for (;;) {
        p = json_skip_ws(p);
        if (*p == '}' || *p != '"') {
            return NULL;
        }
        const char *after_key = NULL;
        char *name = json_read_string(p, &after_key);
        if (!name) {
            return NULL;
        }
        int match = strcmp(name, key) == 0;
        free(name);

        p = json_skip_ws(after_key);
        if (*p != ':') {
            return NULL;
        }
        p = json_skip_ws(p + 1);
        if (match) {
            return p;
        }
        p = json_skip_value(p);
        if (!p) {
            return NULL;
        }
        p = json_skip_ws(p);
        if (*p == ',') {
            p++;
        } else {
            return NULL;
        }
    }
}

char *json_get_str_alloc(const char *json, const char *key) {
    const char *v = json_find_value(json, key);
    if (!v || *v != '"') {
        return NULL;
    }
    return json_read_string(v, NULL);
}

/* Returns 1 only if the key holds a string that fits; a truncated value
 * could name a different file, so it counts as missing. */
int json_get_str(const char *json, const char *key, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    char *value = json_get_str_alloc(json, key);
    if (!value) {
        return 0;
    }
    size_t len = strlen(value);
    if (len >= out_size) {
        free(value);
        return 0;
    }
    memcpy(out, value, len + 1);
    free(value);
    return 1;
}

int json_get_long(const char *json, const char *key, long *out) {
    const char *v = json_find_value(json, key);
    if (!v || !(*v == '-' || isdigit((unsigned char)*v))) {
        return 0;
    }
    char *end = NULL;
    long value = strtol(v, &end, 10);
    if (end == v) {
        return 0;
    }
    *out = value;
    return 1;
}

int json_get_bool(const char *json, const char *key) {
    const char *v = json_find_value(json, key);
    return v && strncmp(v, "true", 4) == 0;
}
