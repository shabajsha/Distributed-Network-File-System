#include "nm_metadata.h"
#include "nm_cache.h"
#include "nm_logging.h"

static unsigned int hash_function(const char *str) {
    unsigned int hash = 5381;
    int c;
    while (str && (c = *str++)) {
        hash = ((hash << 5) + hash) + (unsigned int)c;
    }
    return hash % HASH_BUCKETS;
}

/* Finds a file whatever its state, including one still being created. */
FileMetadata *find_any_file(const char *filename) {
    if (!filename) {
        return NULL;
    }
    HashNode *node = file_hash_table[hash_function(filename)];
    while (node) {
        if (strcmp(node->key, filename) == 0) {
            return node->file;
        }
        node = node->next;
    }
    return NULL;
}

FileMetadata *lookup_file(const char *filename) {
    if (!filename) {
        return NULL;
    }

    FileMetadata *cached = cache_get(filename);
    if (cached && !cached->pending) {
        return cached;
    }

    FileMetadata *file = find_any_file(filename);
    if (file && !file->pending) {
        cache_put(filename, file);
        return file;
    }
    return NULL;
}

void insert_file(FileMetadata *file) {
    if (!file) {
        return;
    }

    unsigned int index = hash_function(file->filename);
    HashNode *new_node = malloc(sizeof(HashNode));
    if (!new_node) {
        return;
    }

    strncpy(new_node->key, file->filename, sizeof(new_node->key) - 1);
    new_node->key[sizeof(new_node->key) - 1] = '\0';
    new_node->file = file;
    new_node->next = file_hash_table[index];
    file_hash_table[index] = new_node;
    if (!file->pending) {
        cache_put(file->filename, file);
    }
}

void mark_file_ready(FileMetadata *file) {
    file->pending = 0;
    cache_put(file->filename, file);
}

static void free_access_list(AccessEntry *entry) {
    while (entry) {
        AccessEntry *next = entry->next;
        free(entry);
        entry = next;
    }
}

void remove_file(const char *filename) {
    cache_remove(filename);

    HashNode **link = &file_hash_table[hash_function(filename)];
    while (*link) {
        if (strcmp((*link)->key, filename) == 0) {
            HashNode *node = *link;
            *link = node->next;
            free_access_list(node->file->access_list);
            free(node->file);
            free(node);
            return;
        }
        link = &(*link)->next;
    }
}

/* The owner can do anything; "W" access also allows reading. */
int check_access(FileMetadata *file, const char *username, const char *required_mode) {
    if (!file || !username || !required_mode) {
        return 0;
    }

    if (strcmp(file->owner, username) == 0) {
        return 1;
    }

    for (AccessEntry *entry = file->access_list; entry; entry = entry->next) {
        if (strcmp(entry->username, username) == 0) {
            if (strcmp(required_mode, "R") == 0) {
                return strchr(entry->mode, 'R') != NULL || strchr(entry->mode, 'W') != NULL;
            }
            if (strcmp(required_mode, "W") == 0) {
                return strchr(entry->mode, 'W') != NULL;
            }
        }
    }
    return 0;
}

int user_index(const char *username) {
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, username) == 0) {
            return i;
        }
    }
    return -1;
}

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

/* Write to a temp file, fsync, then rename over the old file, so a crash
 * leaves either the old metadata or the new, never half of it. */
static int write_file_atomic(const char *path, const char *content) {
    char tmp_path[1100];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmpXXXXXX", path);
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        return -1;
    }
    fchmod(fd, 0600);
    int rc = dnfs_write_all(fd, content, strlen(content));
    if (rc == 0) {
        rc = fsync(fd);
    }
    if (close(fd) != 0) {
        rc = -1;
    }
    if (rc == 0) {
        rc = rename(tmp_path, path);
    }
    if (rc != 0) {
        unlink(tmp_path);
    }
    return rc;
}

/* save_mutex keeps whole saves in order: the newest snapshot of the tables
 * is always the one written last. */
void save_metadata(void) {
    pthread_mutex_lock(&save_mutex);

    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\n  \"version\": 2,\n  \"users\": [");

    pthread_mutex_lock(&files_mutex);
    pthread_mutex_lock(&users_mutex);
    for (int i = 0; i < user_count; i++) {
        sb_append(&sb, i ? ",\n    {\"name\":" : "\n    {\"name\":");
        sb_append_json(&sb, users[i].username);
        sb_append(&sb, ",\"pass\":");
        sb_append_json(&sb, users[i].pass_hash);
        sb_append(&sb, "}");
    }
    pthread_mutex_unlock(&users_mutex);

    sb_append(&sb, "\n  ],\n  \"files\": {");
    int first = 1;
    for (int i = 0; i < HASH_BUCKETS; i++) {
        for (HashNode *node = file_hash_table[i]; node; node = node->next) {
            FileMetadata *file = node->file;
            if (file->pending) {
                continue;
            }
            sb_append(&sb, first ? "\n    " : ",\n    ");
            first = 0;
            sb_append_json(&sb, file->filename);
            sb_append(&sb, ": {\"owner\":");
            sb_append_json(&sb, file->owner);
            sb_append(&sb, ",\"ss_ip\":");
            sb_append_json(&sb, file->ss_ip);
            sb_appendf(&sb, ",\"ss_port\":%d,\"backup_ss_ip\":", file->ss_port);
            sb_append_json(&sb, file->backup_ss_ip);
            sb_appendf(&sb, ",\"backup_ss_port\":%d,\"primary_stale\":%d,\"backup_stale\":%d"
                            ",\"created_at\":%ld,\"last_modified\":%ld,\"last_accessed\":%ld,\"last_accessed_by\":",
                       file->backup_ss_port, file->primary_stale, file->backup_stale,
                       (long)file->created_at, (long)file->last_modified, (long)file->last_accessed);
            sb_append_json(&sb, file->last_accessed_by);
            sb_appendf(&sb, ",\"words\":%d,\"chars\":%d,\"bytes\":%d,\"access\":[",
                       file->words, file->chars, file->bytes);
            for (AccessEntry *entry = file->access_list; entry; entry = entry->next) {
                sb_append(&sb, entry == file->access_list ? "{\"user\":" : ",{\"user\":");
                sb_append_json(&sb, entry->username);
                sb_append(&sb, ",\"mode\":");
                sb_append_json(&sb, entry->mode);
                sb_append(&sb, "}");
            }
            sb_append(&sb, "]}");
        }
    }
    pthread_mutex_unlock(&files_mutex);

    sb_append(&sb, "\n  }\n}\n");
    char *text = sb_take(&sb);
    if (!text || write_file_atomic(METADATA_FILE, text) != 0) {
        log_message("ERROR", "Failed to save metadata", "-", 0, "system");
    }
    free(text);

    pthread_mutex_unlock(&save_mutex);
}

static char *dup_range(const char *start, const char *end) {
    size_t len = (size_t)(end - start);
    char *out = malloc(len + 1);
    if (out) {
        memcpy(out, start, len);
        out[len] = '\0';
    }
    return out;
}

/* Times are epoch seconds; version 1 files stored local-time strings. */
static time_t parse_time(const char *json, const char *key, time_t fallback) {
    long epoch = 0;
    if (json_get_long(json, key, &epoch)) {
        return (time_t)epoch;
    }
    char text[64];
    if (json_get_str(json, key, text, sizeof(text)) && text[0]) {
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        if (strptime(text, "%Y-%m-%d %H:%M:%S", &tm)) {
            tm.tm_isdst = -1;
            return mktime(&tm);
        }
    }
    return fallback;
}

static void load_user(const char *start, const char *end) {
    char name[MAX_USERNAME] = {0};
    char pass[PASS_HASH_LEN] = {0};

    if (*start == '"') {
        char *legacy = json_read_string(start, NULL);
        if (legacy && strlen(legacy) < sizeof(name)) {
            strcpy(name, legacy);
        }
        free(legacy);
    } else {
        char *obj = dup_range(start, end);
        if (obj) {
            json_get_str(obj, "name", name, sizeof(name));
            json_get_str(obj, "pass", pass, sizeof(pass));
        }
        free(obj);
    }

    if (!dnfs_valid_username(name) || user_count >= MAX_USERS || user_index(name) >= 0) {
        return;
    }
    strcpy(users[user_count].username, name);
    strcpy(users[user_count].pass_hash, pass);
    user_count++;
}

static AccessEntry *load_access_list(const char *entry) {
    AccessEntry *head = NULL;
    AccessEntry **tail = &head;

    const char *p = json_find_value(entry, "access");
    if (!p || *p != '[') {
        return NULL;
    }
    p++;
    for (;;) {
        p = json_skip_ws(p);
        if (*p != '{') {
            break;
        }
        const char *end = json_skip_value(p);
        if (!end) {
            break;
        }
        char *obj = dup_range(p, end);
        AccessEntry *new_entry = calloc(1, sizeof(AccessEntry));
        if (obj && new_entry &&
            json_get_str(obj, "user", new_entry->username, sizeof(new_entry->username)) &&
            dnfs_valid_username(new_entry->username)) {
            if (!json_get_str(obj, "mode", new_entry->mode, sizeof(new_entry->mode)) ||
                (strcmp(new_entry->mode, "R") != 0 && strcmp(new_entry->mode, "W") != 0)) {
                strcpy(new_entry->mode, "R");
            }
            *tail = new_entry;
            tail = &new_entry->next;
        } else {
            free(new_entry);
        }
        free(obj);

        p = json_skip_ws(end);
        if (*p == ',') {
            p++;
        }
    }
    return head;
}

static void load_file_entry(const char *filename, const char *entry) {
    if (!dnfs_valid_filename(filename) || find_any_file(filename)) {
        return;
    }
    FileMetadata *file = calloc(1, sizeof(FileMetadata));
    if (!file) {
        return;
    }
    strcpy(file->filename, filename);
    parse_json_string(entry, "owner", file->owner, sizeof(file->owner));
    parse_json_string(entry, "ss_ip", file->ss_ip, sizeof(file->ss_ip));
    file->ss_port = parse_json_int(entry, "ss_port");
    parse_json_string(entry, "backup_ss_ip", file->backup_ss_ip, sizeof(file->backup_ss_ip));
    file->backup_ss_port = parse_json_int(entry, "backup_ss_port");
    file->primary_stale = parse_json_int(entry, "primary_stale");
    file->backup_stale = parse_json_int(entry, "backup_stale");
    file->created_at = parse_time(entry, "created_at", time(NULL));
    file->last_modified = parse_time(entry, "last_modified", file->created_at);
    file->last_accessed = parse_time(entry, "last_accessed", file->created_at);
    parse_json_string(entry, "last_accessed_by", file->last_accessed_by, sizeof(file->last_accessed_by));
    if (!file->last_accessed_by[0]) {
        strcpy(file->last_accessed_by, file->owner);
    }
    file->words = parse_json_int(entry, "words");
    file->chars = parse_json_int(entry, "chars");
    file->bytes = parse_json_int(entry, "bytes");
    file->access_list = load_access_list(entry);
    insert_file(file);
}

/* Each array element and file entry is copied out whole, however large,
 * so long access lists survive a restart. */
void load_metadata(void) {
    FILE *fp = fopen(METADATA_FILE, "r");
    if (!fp) {
        return;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return;
    }
    rewind(fp);

    char *json = malloc((size_t)size + 1);
    if (!json) {
        fclose(fp);
        return;
    }
    size_t read_bytes = fread(json, 1, (size_t)size, fp);
    fclose(fp);
    json[read_bytes] = '\0';

    const char *p = json_find_value(json, "users");
    if (p && *p == '[') {
        p++;
        for (;;) {
            p = json_skip_ws(p);
            if (*p != '"' && *p != '{') {
                break;
            }
            const char *end = json_skip_value(p);
            if (!end) {
                break;
            }
            load_user(p, end);
            p = json_skip_ws(end);
            if (*p == ',') {
                p++;
            }
        }
    }

    p = json_find_value(json, "files");
    if (p && *p == '{') {
        p++;
        for (;;) {
            p = json_skip_ws(p);
            if (*p != '"') {
                break;
            }
            const char *after_key = NULL;
            char *filename = json_read_string(p, &after_key);
            if (!filename) {
                break;
            }
            p = json_skip_ws(after_key);
            if (*p != ':') {
                free(filename);
                break;
            }
            p = json_skip_ws(p + 1);
            const char *end = json_skip_value(p);
            if (!end) {
                free(filename);
                break;
            }
            if (*p == '{') {
                char *entry = dup_range(p, end);
                if (entry) {
                    load_file_entry(filename, entry);
                }
                free(entry);
            }
            free(filename);
            p = json_skip_ws(end);
            if (*p == ',') {
                p++;
            }
        }
    }

    free(json);
}
