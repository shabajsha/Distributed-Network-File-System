#include "ss_file_ops.h"
#include <sys/stat.h>
#include <dirent.h>

// Global path variables (defined here)
char BASE_DIR[1024] = "./storage_server";
char DATA_DIR[1024];
char SNAP_DIR[1024];
char LOG_DIR[1024];

void ensure_directories(void) {
    // Initialize path variables
    snprintf(DATA_DIR, sizeof(DATA_DIR), "%s/data/", BASE_DIR);
    snprintf(SNAP_DIR, sizeof(SNAP_DIR), "%s/snapshots/", BASE_DIR);
    snprintf(LOG_DIR, sizeof(LOG_DIR), "%s/logs", BASE_DIR);
    
    mkdir(BASE_DIR, 0777);
    mkdir(DATA_DIR, 0777);
    mkdir(SNAP_DIR, 0777);
    mkdir(LOG_DIR, 0755);
}

void build_filepath(char *dest, const char *filename) {
    snprintf(dest, 512, "%s%s", DATA_DIR, filename);
}

void build_snapshot_path(char *dest, const char *filename) {
    snprintf(dest, 1024, "%s%s.bak", SNAP_DIR, filename);
}

int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

char *load_file(const char *filename) {
    char path[1024];
    build_filepath(path, filename);

    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

void save_file(const char *filename, const char *content) {
    char path[1024];
    build_filepath(path, filename);

    FILE *f = fopen(path, "w");
    fprintf(f, "%s", content);
    fclose(f);
}

void save_snapshot(const char *filename, const char *content) {
    char path[1024];
    build_snapshot_path(path, filename);

    FILE *f = fopen(path, "w");
    fprintf(f, "%s", content);
    fclose(f);
}

int save_file_atomic(const char *filename, const char *content) {
    if (!filename || !content) return -1;

    char path[1024];
    char tmp_path[1024];
    build_filepath(path, filename);
    snprintf(tmp_path, sizeof(tmp_path), "%s%s.tmp", DATA_DIR, filename);

    FILE *tmp = fopen(tmp_path, "w");
    if (!tmp) return -1;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, tmp);
    fflush(tmp);
    if (fclose(tmp) != 0 || written != len) {
        remove(tmp_path);
        return -1;
    }

    if (remove(path) != 0 && errno != ENOENT) {
        remove(tmp_path);
        return -1;
    }

    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return -1;
    }
    return 0;
}

char *build_files_manifest(void) {
    DIR *dir = opendir(DATA_DIR);
    if (!dir) {
        return strdup("[]");
    }

    size_t capacity = 256;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        closedir(dir);
        return strdup("[]");
    }
    buffer[0] = '\0';
    strcat(buffer, "[");

    struct dirent *entry;
    int first = 1;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[1024];
        snprintf(path, sizeof(path), "%s%s", DATA_DIR, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        size_t needed = strlen(buffer) + strlen(entry->d_name) + 5;
        if (needed >= capacity) {
            capacity *= 2;
            char *tmp = (char *)realloc(buffer, capacity);
            if (!tmp) {
                free(buffer);
                closedir(dir);
                return strdup("[]");
            }
            buffer = tmp;
        }

        if (!first) {
            strcat(buffer, ",");
        }
        first = 0;

        strcat(buffer, "\"");
        strcat(buffer, entry->d_name);
        strcat(buffer, "\"");
    }

    strcat(buffer, "]");
    closedir(dir);
    return buffer;
}
