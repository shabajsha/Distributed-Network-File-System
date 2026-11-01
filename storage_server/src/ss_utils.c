#include "ss_utils.h"

// Global variables
char NM_IP[INET_ADDRSTRLEN] = "127.0.0.1";
char ADVERTISE_IP[INET_ADDRSTRLEN] = "";
int CLIENT_PORT = 9100;
char SS_SECRET[SECRET_LEN];

// Thread-local logging context
__thread ClientLogContext g_log_ctx;

static FILE *g_log_file = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

int json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    return json && key && json_get_str(json, key, out, out_size);
}

int json_get_int(const char *json, const char *key, int *out) {
    long value = 0;
    if (!json || !key || !out || !json_get_long(json, key, &value)) return 0;
    *out = (int)value;
    return 1;
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
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) {
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }

    time_t now = time(NULL);
    struct tm tm_info;
    char filename[64] = "ss.log";
    if (localtime_r(&now, &tm_info)) {
        strftime(filename, sizeof(filename), "ss_%Y%m%d_%H%M%S.log", &tm_info);
    }
    char full_path[700];
    snprintf(full_path, sizeof(full_path), "%s/%s", LOG_DIR, filename);
    g_log_file = fopen(full_path, "a");

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
    struct tm tm_info;
    if (localtime_r(&now, &tm_info)) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);
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
