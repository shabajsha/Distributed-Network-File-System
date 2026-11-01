#include "ss_file_ops.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

// Global path variables (defined here)
char BASE_DIR[256] = "./storage_server";
char DATA_DIR[512];
char SNAP_DIR[512];
char LOG_DIR[512];

void ensure_directories(void) {
    // Initialize path variables
    snprintf(DATA_DIR, sizeof(DATA_DIR), "%s/data/", BASE_DIR);
    snprintf(SNAP_DIR, sizeof(SNAP_DIR), "%s/snapshots/", BASE_DIR);
    snprintf(LOG_DIR, sizeof(LOG_DIR), "%s/logs", BASE_DIR);

    mkdir(BASE_DIR, 0755);
    mkdir(DATA_DIR, 0755);
    mkdir(SNAP_DIR, 0755);
    mkdir(LOG_DIR, 0755);
}

void build_filepath(char *dest, size_t size, const char *filename) {
    snprintf(dest, size, "%s%s", DATA_DIR, filename);
}

void build_snapshot_path(char *dest, size_t size, const char *filename) {
    snprintf(dest, size, "%s%s.bak", SNAP_DIR, filename);
}

int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

static char *read_whole_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }

    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }

    size_t total = 0;
    while (total < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + total, (size_t)st.st_size - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
    buf[total] = '\0';
    return buf;
}

char *load_file(const char *filename) {
    char path[1024];
    build_filepath(path, sizeof(path), filename);
    return read_whole_file(path);
}

char *load_snapshot(const char *filename) {
    char path[1024];
    build_snapshot_path(path, sizeof(path), filename);
    return read_whole_file(path);
}

// Writes a uniquely named temp file in the same directory, flushes it to
// disk, then renames it over the target. rename() replaces atomically, so
// readers see the old file or the new one, never a missing or partial one.
// Temp names start with '.', which no valid filename does.
static int write_atomic(const char *dir, const char *final_name, const char *content) {
    char tmp_path[1024];
    char final_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.%s.XXXXXX", dir, final_name);
    snprintf(final_path, sizeof(final_path), "%s%s", dir, final_name);

    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;
    fchmod(fd, 0644);

    int rc = dnfs_write_all(fd, content, strlen(content));
    if (rc == 0) rc = fsync(fd);
    if (close(fd) != 0) rc = -1;
    if (rc == 0) rc = rename(tmp_path, final_path);
    if (rc != 0) unlink(tmp_path);
    return rc;
}

int save_file_atomic(const char *filename, const char *content) {
    if (!filename || !content) return -1;
    return write_atomic(DATA_DIR, filename, content);
}

int save_snapshot(const char *filename, const char *content) {
    if (!filename || !content) return -1;
    char name[MAX_FILENAME + 8];
    snprintf(name, sizeof(name), "%s.bak", filename);
    return write_atomic(SNAP_DIR, name, content);
}

void remove_snapshot(const char *filename) {
    char path[1024];
    build_snapshot_path(path, sizeof(path), filename);
    remove(path);
}

char *build_files_manifest(void) {
    DIR *dir = opendir(DATA_DIR);
    if (!dir) {
        return strdup("[]");
    }

    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "[");

    struct dirent *entry;
    int first = 1;
    while ((entry = readdir(dir)) != NULL) {
        // Skips ".", "..", temp files and anything else that isn't a valid name
        if (!dnfs_valid_filename(entry->d_name)) {
            continue;
        }

        char path[1024];
        build_filepath(path, sizeof(path), entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        if (!first) {
            sb_append(&sb, ",");
        }
        first = 0;
        sb_append_json(&sb, entry->d_name);
    }
    closedir(dir);

    sb_append(&sb, "]");
    char *manifest = sb_take(&sb);
    return manifest ? manifest : strdup("[]");
}
