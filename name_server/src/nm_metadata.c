#include "nm_metadata.h"
#include "nm_cache.h"

static unsigned int hash_function(const char *str) {
    unsigned int hash = 5381;
    int c;
    while (str && (c = *str++)) {
        hash = ((hash << 5) + hash) + (unsigned int)c;
    }
    return hash % MAX_FILES;
}

FileMetadata *lookup_file(const char *filename) {
    if (!filename) {
        return NULL;
    }

    FileMetadata *cached = cache_get(filename);
    if (cached && cached->active) {
        return cached;
    }

    unsigned int index = hash_function(filename);
    HashNode *node = file_hash_table[index];

    while (node) {
        if (strcmp(node->key, filename) == 0 && node->file->active) {
            cache_put(filename, node->file);
            return node->file;
        }
        node = node->next;
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
    cache_put(file->filename, file);
}

int check_access(FileMetadata *file, const char *username, const char *required_mode) {
    if (!file || !username || !required_mode) {
        return 0;
    }

    if (strcmp(file->owner, username) == 0) {
        return 1;
    }

    AccessEntry *entry = file->access_list;
    while (entry) {
        if (strcmp(entry->username, username) == 0) {
            if (strcmp(required_mode, "R") == 0) {
                return (strstr(entry->mode, "R") != NULL || strstr(entry->mode, "W") != NULL);
            }
            if (strcmp(required_mode, "W") == 0) {
                return (strstr(entry->mode, "W") != NULL);
            }
        }
        entry = entry->next;
    }
    return 0;
}

static const char *skip_ws(const char *p) {
    while (p && *p && (unsigned char)*p <= ' ') {
        p++;
    }
    return p;
}

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

    pos = skip_ws(pos);
    if (*pos != '"') {
        return;
    }
    pos++;

    const char *end = pos;
    while (*end && *end != '"') {
        if (*end == '\\' && *(end + 1)) {
            end += 2;
            continue;
        }
        end++;
    }
    if (*end != '"') {
        return;
    }

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

    pos = skip_ws(pos);
    return atoi(pos);
}

int request_file_stats(const char *ip, int port, const char *filename,
                       int *words, int *chars, int *bytes) {
    if (!ip || !filename) {
        return 0;
    }

    int ss_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_fd < 0) {
        return 0;
    }

    struct sockaddr_in ss_addr;
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &ss_addr.sin_addr) <= 0) {
        close(ss_fd);
        return 0;
    }

    if (connect(ss_fd, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
        close(ss_fd);
        return 0;
    }

    char stat_req[256];
    int len = snprintf(stat_req, sizeof(stat_req),
                       "{\"cmd\":\"STAT\",\"filename\":\"%s\"}\n",
                       filename);
    if (len <= 0) {
        close(ss_fd);
        return 0;
    }

    if (send(ss_fd, stat_req, len, 0) < 0) {
        close(ss_fd);
        return 0;
    }

    char stat_resp[512];
    int total = 0;
    int done = 0;
    while (!done && total < (int)sizeof(stat_resp) - 1) {
        int chunk = recv(ss_fd, stat_resp + total, (int)sizeof(stat_resp) - 1 - total, 0);
        if (chunk <= 0) {
            break;
        }
        total += chunk;
        for (int i = total - chunk; i < total; i++) {
            if (stat_resp[i] == '\n') {
                done = 1;
                total = i + 1;
                break;
            }
        }
    }
    close(ss_fd);

    if (total <= 0) {
        return 0;
    }

    stat_resp[total] = '\0';
    if (total > 0 && stat_resp[total - 1] == '\n') {
        stat_resp[total - 1] = '\0';
    }

    if (!strstr(stat_resp, "\"status\":\"OK\"")) {
        return 0;
    }

    if (words) *words = parse_json_int(stat_resp, "words");
    if (chars) *chars = parse_json_int(stat_resp, "chars");
    if (bytes) *bytes = parse_json_int(stat_resp, "bytes");
    return 1;
}

void save_metadata(void) {
    FILE *fp = fopen(METADATA_FILE, "w");
    if (!fp) {
        return;
    }

    fprintf(fp, "{\n  \"users\": [");

    int first = 1;
    for (int i = 0; i < client_count; i++) {
        if (!first) {
            fprintf(fp, ",");
        }
        first = 0;
        fprintf(fp, "\"%s\"", clients[i].username);
    }

    fprintf(fp, "],\n  \"files\": {\n");

    first = 1;
    for (int i = 0; i < MAX_FILES; i++) {
        HashNode *node = file_hash_table[i];
        while (node) {
            FileMetadata *file = node->file;
            if (file->active) {
                if (!first) {
                    fprintf(fp, ",\n");
                }
                first = 0;

                char created_str[64];
                char modified_str[64];
                char accessed_str[64];

                struct tm *tm_info = localtime(&file->created_at);
                strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M:%S", tm_info);

                tm_info = localtime(&file->last_modified);
                strftime(modified_str, sizeof(modified_str), "%Y-%m-%d %H:%M:%S", tm_info);

                tm_info = localtime(&file->last_accessed);
                strftime(accessed_str, sizeof(accessed_str), "%Y-%m-%d %H:%M:%S", tm_info);

                fprintf(fp, "    \"%s\": {\"owner\": \"%s\", \"ss_ip\": \"%s\", \"ss_port\": %d, \"backup_ss_ip\": \"%s\", \"backup_ss_port\": %d, \"created_at\": \"%s\", \"last_modified\": \"%s\", \"last_accessed\": \"%s\", \"last_accessed_by\": \"%s\", \"words\": %d, \"chars\": %d, \"bytes\": %d, \"access\": [",
                        file->filename, file->owner, file->ss_ip, file->ss_port, file->backup_ss_ip, file->backup_ss_port,
                        created_str, modified_str, accessed_str, file->last_accessed_by, file->words, file->chars, file->bytes);

                AccessEntry *entry = file->access_list;
                int first_access = 1;
                while (entry) {
                    if (!first_access) {
                        fprintf(fp, ", ");
                    }
                    first_access = 0;
                    fprintf(fp, "{\"user\": \"%s\", \"mode\": \"%s\"}", entry->username, entry->mode);
                    entry = entry->next;
                }

                fprintf(fp, "]}");
            }
            node = node->next;
        }
    }

    fprintf(fp, "\n  }\n}\n");
    fclose(fp);
}

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

    const char *users_section = strstr(json, "\"users\"");
    if (users_section) {
        const char *array_start = strchr(users_section, '[');
        if (array_start) {
            const char *cursor = array_start + 1;
            client_count = 0;

            while (*cursor && client_count < MAX_CLIENTS) {
                while (*cursor && (isspace((unsigned char)*cursor) || *cursor == ',')) {
                    cursor++;
                }
                if (*cursor == ']' || *cursor == '\0') {
                    break;
                }
                if (*cursor == '"') {
                    cursor++;
                    const char *name_end = strchr(cursor, '"');
                    if (name_end) {
                        size_t name_len = (size_t)(name_end - cursor);
                        if (name_len >= MAX_USERNAME) {
                            name_len = MAX_USERNAME - 1;
                        }
                        memcpy(clients[client_count].username, cursor, name_len);
                        clients[client_count].username[name_len] = '\0';
                        clients[client_count].active = 0;
                        client_count++;
                        cursor = name_end + 1;
                    } else {
                        break;
                    }
                } else {
                    cursor++;
                }
            }
        }
    }

    const char *files_section = strstr(json, "\"files\"");
    if (files_section) {
        const char *cursor = strchr(files_section, '{');
        if (cursor) {
            cursor++;
            while (*cursor) {
                while (*cursor && (isspace((unsigned char)*cursor) || *cursor == ',')) {
                    cursor++;
                }
                if (*cursor == '}' || *cursor == '\0') {
                    break;
                }
                if (*cursor != '"') {
                    cursor++;
                    continue;
                }
                cursor++;
                const char *fname_end = strchr(cursor, '"');
                if (!fname_end) {
                    break;
                }
                char filename[MAX_FILENAME];
                size_t fname_len = (size_t)(fname_end - cursor);
                if (fname_len >= sizeof(filename)) {
                    fname_len = sizeof(filename) - 1;
                }
                memcpy(filename, cursor, fname_len);
                filename[fname_len] = '\0';

                const char *entry_start = strchr(fname_end, '{');
                if (!entry_start) {
                    break;
                }
                entry_start++;
                int brace_depth = 1;
                const char *entry_end = entry_start;
                while (*entry_end && brace_depth > 0) {
                    if (*entry_end == '{') {
                        brace_depth++;
                    } else if (*entry_end == '}') {
                        brace_depth--;
                    }
                    entry_end++;
                }
                if (brace_depth != 0) {
                    break;
                }
                size_t entry_len = (size_t)((entry_end - 1) - entry_start);
                if (entry_len >= 1024) {
                    entry_len = 1023;
                }
                char entry_buf[1024];
                memcpy(entry_buf, entry_start, entry_len);
                entry_buf[entry_len] = '\0';

                FileMetadata *file = malloc(sizeof(FileMetadata));
                if (!file) {
                    break;
                }
                memset(file, 0, sizeof(*file));
                strncpy(file->filename, filename, sizeof(file->filename) - 1);
                file->filename[sizeof(file->filename) - 1] = '\0';
                parse_json_string(entry_buf, "owner", file->owner, sizeof(file->owner));
                parse_json_string(entry_buf, "ss_ip", file->ss_ip, sizeof(file->ss_ip));
                file->ss_port = parse_json_int(entry_buf, "ss_port");
                parse_json_string(entry_buf, "backup_ss_ip", file->backup_ss_ip, sizeof(file->backup_ss_ip));
                file->backup_ss_port = parse_json_int(entry_buf, "backup_ss_port");
                file->active = 1;

                char created_str[64] = {0};
                char modified_str[64] = {0};
                char accessed_str[64] = {0};
                char accessed_by[MAX_USERNAME] = {0};

                parse_json_string(entry_buf, "created_at", created_str, sizeof(created_str));
                parse_json_string(entry_buf, "last_modified", modified_str, sizeof(modified_str));
                parse_json_string(entry_buf, "last_accessed", accessed_str, sizeof(accessed_str));
                parse_json_string(entry_buf, "last_accessed_by", accessed_by, sizeof(accessed_by));

                if (strlen(created_str) > 0) {
                    struct tm tm = {0};
                    strptime(created_str, "%Y-%m-%d %H:%M:%S", &tm);
                    file->created_at = mktime(&tm);
                } else {
                    file->created_at = time(NULL);
                }

                if (strlen(modified_str) > 0) {
                    struct tm tm = {0};
                    strptime(modified_str, "%Y-%m-%d %H:%M:%S", &tm);
                    file->last_modified = mktime(&tm);
                } else {
                    file->last_modified = file->created_at;
                }

                if (strlen(accessed_str) > 0) {
                    struct tm tm = {0};
                    strptime(accessed_str, "%Y-%m-%d %H:%M:%S", &tm);
                    file->last_accessed = mktime(&tm);
                } else {
                    file->last_accessed = file->created_at;
                }

                if (strlen(accessed_by) > 0) {
                    strncpy(file->last_accessed_by, accessed_by, sizeof(file->last_accessed_by) - 1);
                    file->last_accessed_by[sizeof(file->last_accessed_by) - 1] = '\0';
                } else {
                    strncpy(file->last_accessed_by, file->owner, sizeof(file->last_accessed_by) - 1);
                    file->last_accessed_by[sizeof(file->last_accessed_by) - 1] = '\0';
                }

                file->words = parse_json_int(entry_buf, "words");
                file->chars = parse_json_int(entry_buf, "chars");
                file->bytes = parse_json_int(entry_buf, "bytes");
                file->access_list = NULL;

                AccessEntry *head = NULL;
                AccessEntry **tail = &head;

                const char *access_pos = strstr(entry_buf, "\"access\"");
                if (access_pos) {
                    const char *array_start = strchr(access_pos, '[');
                    if (array_start) {
                        array_start++;
                        while (*array_start) {
                            while (*array_start && (isspace((unsigned char)*array_start) || *array_start == ',')) {
                                array_start++;
                            }
                            if (*array_start == ']' || *array_start == '\0') {
                                break;
                            }
                            if (*array_start != '{') {
                                array_start++;
                                continue;
                            }

                            array_start++;
                            int obj_depth = 1;
                            const char *obj_cursor = array_start;
                            while (*obj_cursor && obj_depth > 0) {
                                if (*obj_cursor == '{') {
                                    obj_depth++;
                                } else if (*obj_cursor == '}') {
                                    obj_depth--;
                                }
                                obj_cursor++;
                            }
                            if (obj_depth != 0) {
                                break;
                            }

                            size_t obj_len = (size_t)((obj_cursor - 1) - array_start);
                            if (obj_len >= 256) {
                                obj_len = 255;
                            }
                            char access_obj[256];
                            memcpy(access_obj, array_start, obj_len);
                            access_obj[obj_len] = '\0';

                            AccessEntry *new_entry = malloc(sizeof(AccessEntry));
                            if (!new_entry) {
                                break;
                            }
                            memset(new_entry, 0, sizeof(*new_entry));
                            parse_json_string(access_obj, "user", new_entry->username, sizeof(new_entry->username));
                            parse_json_string(access_obj, "mode", new_entry->mode, sizeof(new_entry->mode));
                            if (new_entry->username[0] == '\0') {
                                free(new_entry);
                                array_start = obj_cursor;
                                continue;
                            }
                            if (new_entry->mode[0] == '\0') {
                                strcpy(new_entry->mode, "R");
                            }
                            new_entry->next = NULL;
                            *tail = new_entry;
                            tail = &new_entry->next;

                            array_start = obj_cursor;
                        }
                    }
                }

                file->access_list = head;
                insert_file(file);
                cursor = entry_end;
            }
        }
    }

    free(json);
}
