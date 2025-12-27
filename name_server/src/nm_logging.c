#include "nm_logging.h"

void log_message(const char *level, const char *message, const char *ip, int port, const char *username) {
    pthread_mutex_lock(&log_mutex);

    if (!log_file) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    time_t now = time(NULL);
    char timestamp[64];
    struct tm *tm_info = localtime(&now);
    if (tm_info) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        snprintf(timestamp, sizeof(timestamp), "unknown");
    }

    fprintf(log_file, "[%s] [%s] IP=%s PORT=%d USER=%s MSG=%s\n",
            timestamp,
            level ? level : "INFO",
            ip ? ip : "-",
            port,
            username ? username : "-",
            message ? message : "-");
    fflush(log_file);

    if (level && (strcmp(level, "ERROR") == 0 || strcmp(level, "INFO") == 0)) {
        printf("[%s] [%s] %s\n", timestamp, level, message ? message : "-");
    }

    pthread_mutex_unlock(&log_mutex);
}
