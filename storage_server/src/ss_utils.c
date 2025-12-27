#include "ss_utils.h"

// Global variables
char NM_IP[INET_ADDRSTRLEN] = "127.0.0.1";
char ADVERTISE_IP[INET_ADDRSTRLEN] = "";
int CLIENT_PORT = 9100;

// Thread-local logging context
__thread ClientLogContext g_log_ctx;

static FILE *g_log_file = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *skip_spaces(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

int json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return 0;

    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;

    p += strlen(pattern);
    p = skip_spaces(p);
    if (!p || *p != ':') return 0;
    p++;
    p = skip_spaces(p);
    if (!p || *p != '"') return 0;
    p++;

    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p + 1)) {
            p += 2;
            continue;
        }
        p++;
    }
    if (*p != '"') return 0;

    // Unescape the string properly
    size_t idx = 0;
    p = start;
    while (*p && *p != '"' && idx < out_size - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n':  out[idx++] = '\n'; break;
                case 'r':  out[idx++] = '\r'; break;
                case 't':  out[idx++] = '\t'; break;
                case '\\': out[idx++] = '\\'; break;
                case '"':  out[idx++] = '"'; break;
                case '/':  out[idx++] = '/'; break;
                default:   out[idx++] = *p; break;
            }
            p++;
        } else {
            out[idx++] = *p++;
        }
    }
    out[idx] = '\0';
    return 1;
}

int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return 0;

    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;

    p += strlen(pattern);
    p = skip_spaces(p);
    if (!p || *p != ':') return 0;
    p++;
    p = skip_spaces(p);
    if (!p) return 0;

    char *endptr = NULL;
    long val = strtol(p, &endptr, 10);
    if (p == endptr) return 0;
    *out = (int)val;
    return 1;
}

char *json_escape(const char *src) {
    if (!src) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    size_t extra = 0;
    for (const char *p = src; *p; p++) {
        switch (*p) {
            case '\\':
            case '"':
            case '\n':
            case '\r':
            case '\t':
                extra++;
                break;
            default:
                if ((unsigned char)*p < 0x20) {
                    extra += 5;
                }
                break;
        }
    }

    size_t len = strlen(src);
    char *out = malloc(len + extra + 1);
    if (!out) return NULL;

    char *w = out;
    for (const char *p = src; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        switch (ch) {
            case '\\': *w++ = '\\'; *w++ = '\\'; break;
            case '"': *w++ = '\\'; *w++ = '"'; break;
            case '\n': *w++ = '\\'; *w++ = 'n'; break;
            case '\r': *w++ = '\\'; *w++ = 'r'; break;
            case '\t': *w++ = '\\'; *w++ = 't'; break;
            default:
                if (ch < 0x20) {
                    sprintf(w, "\\u%04x", ch);
                    w += 6;
                } else {
                    *w++ = (char)ch;
                }
                break;
        }
    }
    *w = '\0';
    return out;
}

int is_loopback_address(const char *ip) {
    if (!ip || !*ip) return 1;
    return (strcmp(ip, "127.0.0.1") == 0 || strcmp(ip, "0.0.0.0") == 0);
}

int choose_non_loopback_ipv4(char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;

    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == -1) {
        return 0;
    }

    int found = 0;
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (ifa->ifa_flags & IFF_LOOPBACK) {
            continue;
        }

        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        char candidate[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sa->sin_addr, candidate, sizeof(candidate))) {
            continue;
        }

        if (is_loopback_address(candidate)) {
            continue;
        }

        strncpy(out, candidate, out_size - 1);
        out[out_size - 1] = '\0';
        found = 1;
        break;
    }

    freeifaddrs(ifaddr);
    return found;
}

void init_logging(void) {
    extern char LOG_DIR[1024];
    
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) {
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[1024];
    if (tm_info) {
        strftime(filename, sizeof(filename), "ss_%Y%m%d_%H%M%S.log", tm_info);
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", LOG_DIR, filename);
        g_log_file = fopen(full_path, "a");
    } else {
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/ss.log", LOG_DIR);
        g_log_file = fopen(full_path, "a");
    }

    if (!g_log_file) {
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }

    setvbuf(g_log_file, NULL, _IONBF, 0);
    pthread_mutex_unlock(&g_log_mutex);
}

void log_event(const char *level, const char *ip, int port, const char *username, 
               const char *cmd, const char *payload) {
    if (!g_log_file) {
        return;
    }

    pthread_mutex_lock(&g_log_mutex);
    time_t now = time(NULL);
    char timestamp[64];
    struct tm *tm_info = localtime(&now);
    if (tm_info) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        strcpy(timestamp, "unknown");
    }

    fprintf(g_log_file, "[%s] [%s] IP=%s PORT=%d USER=%s CMD=%s MSG=%s\n",
            timestamp,
            level ? level : "INFO",
            ip ? ip : "-",
            port,
            (username && *username) ? username : "-",
            (cmd && *cmd) ? cmd : "-",
            payload ? payload : "-");
    pthread_mutex_unlock(&g_log_mutex);
}
