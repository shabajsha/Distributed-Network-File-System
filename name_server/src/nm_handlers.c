#include "nm_handlers.h"
#include "nm_cache.h"
#include "nm_logging.h"
#include "nm_metadata.h"
#include "nm_network.h"

void handle_register_client(int client_fd, const char *request, const char *client_ip) {
    char username[MAX_USERNAME] = {0};
    parse_json_string(request, "username", username, sizeof(username));

    pthread_mutex_lock(&clients_mutex);

    int existing_index = -1;
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].username, username) == 0) {
            existing_index = i;
            break;
        }
    }

    if (existing_index >= 0) {
        strncpy(clients[existing_index].ip, client_ip, sizeof(clients[existing_index].ip) - 1);
        clients[existing_index].ip[sizeof(clients[existing_index].ip) - 1] = '\0';
        clients[existing_index].active = 1;
        clients[existing_index].connected_at = time(NULL);

        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Client re-registered: %s", username);
        log_message("INFO", log_msg, client_ip, 0, username);

        send_response(client_fd, "{\"status\":\"OK\",\"msg\":\"Registered successfully\"}");
    } else if (client_count < MAX_CLIENTS) {
        strncpy(clients[client_count].username, username, sizeof(clients[client_count].username) - 1);
        clients[client_count].username[sizeof(clients[client_count].username) - 1] = '\0';
        clients[client_count].socket_fd = client_fd;
        strncpy(clients[client_count].ip, client_ip, sizeof(clients[client_count].ip) - 1);
        clients[client_count].ip[sizeof(clients[client_count].ip) - 1] = '\0';
        clients[client_count].active = 1;
        clients[client_count].connected_at = time(NULL);
        client_count++;

        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Client registered: %s", username);
        log_message("INFO", log_msg, client_ip, 0, username);

        send_response(client_fd, "{\"status\":\"OK\",\"msg\":\"Registered successfully\"}");
    } else {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"MAX_CLIENTS_REACHED\"}");
    }

    pthread_mutex_unlock(&clients_mutex);
}

void handle_register_ss(int ss_fd, const char *request, const char *ss_ip) {
    char advertised_ip[INET_ADDRSTRLEN] = {0};
    parse_json_string(request, "ip", advertised_ip, sizeof(advertised_ip));
    int nm_port = parse_json_int(request, "nm_port");
    int client_port = parse_json_int(request, "client_port");

    char resolved_ip[INET_ADDRSTRLEN] = {0};
    int advertised_valid = 0;
    if (advertised_ip[0] != '\0') {
        struct sockaddr_in tmp;
        advertised_valid = (inet_pton(AF_INET, advertised_ip, &tmp.sin_addr) == 1);
    }

    int use_observed_ip = 0;
    if (!advertised_valid || strcmp(advertised_ip, "0.0.0.0") == 0) {
        use_observed_ip = 1;
    } else if (strcmp(advertised_ip, "127.0.0.1") == 0 && ss_ip && strcmp(ss_ip, "127.0.0.1") != 0) {
        use_observed_ip = 1;
    } else if (strcmp(advertised_ip, "localhost") == 0 && ss_ip && strcmp(ss_ip, "127.0.0.1") != 0) {
        use_observed_ip = 1;
    }

    if (!use_observed_ip && advertised_valid) {
        strncpy(resolved_ip, advertised_ip, sizeof(resolved_ip) - 1);
    } else if (ss_ip && *ss_ip) {
        strncpy(resolved_ip, ss_ip, sizeof(resolved_ip) - 1);
    }

    if (resolved_ip[0] == '\0') {
        const char *fallback = advertised_ip[0] ? advertised_ip : "127.0.0.1";
        strncpy(resolved_ip, fallback, sizeof(resolved_ip) - 1);
    }
    resolved_ip[sizeof(resolved_ip) - 1] = '\0';

    int used_advertised_ip = (!use_observed_ip && advertised_valid);

    pthread_mutex_lock(&ss_mutex);

    int existing_index = -1;
    for (int i = 0; i < ss_count; i++) {
        if (storage_servers[i].client_port != client_port) {
            continue;
        }
        if ((strcmp(storage_servers[i].ip, resolved_ip) == 0) ||
            (ss_ip && strcmp(storage_servers[i].ip, ss_ip) == 0) ||
            (advertised_ip[0] && strcmp(storage_servers[i].ip, advertised_ip) == 0)) {
            existing_index = i;
            break;
        }
    }

    int ss_index;
    if (existing_index >= 0) {
        ss_index = existing_index;
        storage_servers[ss_index].nm_port = nm_port;
        storage_servers[ss_index].socket_fd = ss_fd;
        storage_servers[ss_index].active = 1;
        storage_servers[ss_index].connected_at = time(NULL);
        strncpy(storage_servers[ss_index].ip, resolved_ip, sizeof(storage_servers[ss_index].ip) - 1);
        storage_servers[ss_index].ip[sizeof(storage_servers[ss_index].ip) - 1] = '\0';

        if (storage_servers[ss_index].files) {
            for (int i = 0; i < storage_servers[ss_index].file_count; i++) {
                free(storage_servers[ss_index].files[i]);
            }
            free(storage_servers[ss_index].files);
            storage_servers[ss_index].files = NULL;
            storage_servers[ss_index].file_count = 0;
        }
    } else if (ss_count < MAX_STORAGE_SERVERS) {
        ss_index = ss_count;
        strncpy(storage_servers[ss_index].ip, resolved_ip, sizeof(storage_servers[ss_index].ip) - 1);
        storage_servers[ss_index].ip[sizeof(storage_servers[ss_index].ip) - 1] = '\0';
        storage_servers[ss_index].nm_port = nm_port;
        storage_servers[ss_index].client_port = client_port;
        storage_servers[ss_index].socket_fd = ss_fd;
        storage_servers[ss_index].active = 1;
        storage_servers[ss_index].connected_at = time(NULL);
        storage_servers[ss_index].files = NULL;
        storage_servers[ss_index].file_count = 0;
        ss_count++;
    } else {
        pthread_mutex_unlock(&ss_mutex);
        send_response(ss_fd, "{\"status\":\"ERR\",\"reason\":\"MAX_SS_REACHED\"}");
        return;
    }

    const char *files_start = strstr(request, "\"files\":[");
    if (files_start) {
        files_start += 9;
        const char *p = files_start;
        while (*p && *p != ']') {
            if (*p == '"') {
                p++;
                const char *start = p;
                while (*p && *p != '"') {
                    if (*p == '\\' && *(p + 1)) {
                        p++;
                    }
                    if (*p) {
                        p++;
                    }
                }
                size_t len = (size_t)(p - start);
                if (len > 0 && len < MAX_FILENAME) {
                    char *filename = malloc(len + 1);
                    if (filename) {
                        strncpy(filename, start, len);
                        filename[len] = '\0';

                        char **tmp = realloc(storage_servers[ss_index].files,
                                             sizeof(char *) * (storage_servers[ss_index].file_count + 1));
                        if (tmp) {
                            storage_servers[ss_index].files = tmp;
                            storage_servers[ss_index].files[storage_servers[ss_index].file_count++] = filename;
                        } else {
                            free(filename);
                        }
                    }
                }
            }
            if (*p) {
                p++;
            }
        }
    }

    if (!used_advertised_ip) {
        char warn_msg[512];
        snprintf(warn_msg, sizeof(warn_msg),
                 "Storage Server advertised '%s' but using observed IP %s",
                 advertised_ip[0] ? advertised_ip : "<none>", resolved_ip);
        log_message("INFO", warn_msg, resolved_ip, client_port, "SS");
    }

    char log_msg[512];
    if (existing_index >= 0) {
        snprintf(log_msg, sizeof(log_msg), "Storage Server re-registered: %s:%d (was offline)", resolved_ip, client_port);
        log_message("INFO", log_msg, resolved_ip, client_port, "SS");

        pthread_mutex_lock(&files_mutex);
        int files_updated = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            HashNode *node = file_hash_table[i];
            while (node) {
                FileMetadata *file = node->file;
                if (file->active && strcmp(file->ss_ip, resolved_ip) == 0 && file->ss_port == client_port) {
                    strncpy(file->ss_ip, resolved_ip, sizeof(file->ss_ip) - 1);
                    file->ss_ip[sizeof(file->ss_ip) - 1] = '\0';
                    file->ss_port = client_port;
                    cache_remove(file->filename);
                    files_updated++;

                    char update_msg[256];
                    snprintf(update_msg, sizeof(update_msg), "Updated file mapping: %s -> %s:%d",
                             file->filename, resolved_ip, client_port);
                    log_message("INFO", update_msg, resolved_ip, client_port, "SS");
                }
                node = node->next;
            }
        }
        pthread_mutex_unlock(&files_mutex);

        snprintf(log_msg, sizeof(log_msg), "Re-registration complete: %d files updated", files_updated);
        log_message("INFO", log_msg, resolved_ip, client_port, "SS");
    } else {
        snprintf(log_msg, sizeof(log_msg), "Storage Server registered: %s:%d", resolved_ip, client_port);
        log_message("INFO", log_msg, resolved_ip, client_port, "SS");
    }

    send_response(ss_fd, "{\"status\":\"OK\",\"msg\":\"SS Registered successfully\"}");
    pthread_mutex_unlock(&ss_mutex);
}

void handle_view(int client_fd, const char *request, const char *username) {
    char flags[16] = {0};
    parse_json_string(request, "flags", flags, sizeof(flags));

    int show_all = (strstr(flags, "a") != NULL);
    int show_details = (strstr(flags, "l") != NULL);

    char response[BUFFER_SIZE] = {0};
    strcpy(response, "{\"status\":\"OK\",\"files\":[");

    pthread_mutex_lock(&files_mutex);

    int first = 1;
    for (int i = 0; i < MAX_FILES; i++) {
        HashNode *node = file_hash_table[i];
        while (node) {
            FileMetadata *file = node->file;
            if (file->active) {
                int has_access = show_all || check_access(file, username, "R");
                if (has_access) {
                    if (!first) {
                        strcat(response, ",");
                    }
                    first = 0;

                    int words = file->words;
                    int chars = file->chars;
                    int bytes = file->bytes;

                    int temp_words = 0;
                    int temp_chars = 0;
                    int temp_bytes = 0;
                    if (request_file_stats(file->ss_ip, file->ss_port, file->filename,
                                           &temp_words, &temp_chars, &temp_bytes)) {
                        words = temp_words;
                        chars = temp_chars;
                        bytes = temp_bytes;
                        file->words = words;
                        file->chars = chars;
                        file->bytes = bytes;
                    }

                    if (show_details) {
                        char entry[512];
                        struct tm *tm_info = localtime(&file->last_accessed);
                        char timestamp[32];
                        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", tm_info);

                        snprintf(entry, sizeof(entry),
                                 "{\"filename\":\"%s\",\"owner\":\"%s\",\"words\":%d,\"chars\":%d,\"bytes\":%d,\"last_accessed\":\"%s\"}",
                                 file->filename, file->owner, words, chars, bytes, timestamp);
                        strcat(response, entry);
                    } else {
                        char entry[300];
                        snprintf(entry, sizeof(entry), "\"%s\"", file->filename);
                        strcat(response, entry);
                    }
                }
            }
            node = node->next;
        }
    }

    strcat(response, "]}");
    pthread_mutex_unlock(&files_mutex);

    log_message("INFO", "VIEW command executed", "0.0.0.0", 0, username);
    send_response(client_fd, response);
}

void handle_list(int client_fd, const char *username) {
    char response[BUFFER_SIZE] = "{\"status\":\"OK\",\"users\":[";

    pthread_mutex_lock(&clients_mutex);

    int first = 1;
    for (int i = 0; i < client_count; i++) {
        if (!first) {
            strcat(response, ",");
        }
        first = 0;

        char entry[128];
        snprintf(entry, sizeof(entry), "\"%s\"", clients[i].username);
        strcat(response, entry);
    }

    strcat(response, "]}");
    pthread_mutex_unlock(&clients_mutex);

    log_message("INFO", "LIST command executed", "0.0.0.0", 0, username);
    send_response(client_fd, response);
}

void handle_create(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    parse_json_string(request, "filename", filename, sizeof(filename));
    if (strlen(filename) == 0) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"BAD_REQUEST\"}");
        return;
    }

    pthread_mutex_lock(&files_mutex);
    if (lookup_file(filename)) {
        pthread_mutex_unlock(&files_mutex);
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"ALREADY_EXISTS\"}");
        return;
    }
    pthread_mutex_unlock(&files_mutex);

    pthread_mutex_lock(&ss_mutex);
    if (ss_count == 0) {
        pthread_mutex_unlock(&ss_mutex);
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"NO_SS_AVAILABLE\"}");
        return;
    }

    char ss_ip[INET_ADDRSTRLEN] = {0};
    int ss_port = 0;
    int ss_fd = -1;
    int success = 0;
    char ss_response[512] = {0};

    static int next_ss_index = 0;
    int start_index = next_ss_index;

    for (int attempt = 0; attempt < ss_count; attempt++) {
        int ss_index = (start_index + attempt) % ss_count;

        if (!storage_servers[ss_index].active) {
            continue;
        }

        strncpy(ss_ip, storage_servers[ss_index].ip, sizeof(ss_ip) - 1);
        ss_ip[sizeof(ss_ip) - 1] = '\0';
        ss_port = storage_servers[ss_index].client_port;

        pthread_mutex_unlock(&ss_mutex);

        ss_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (ss_fd < 0) {
            pthread_mutex_lock(&ss_mutex);
            continue;
        }

        struct sockaddr_in ss_addr;
        ss_addr.sin_family = AF_INET;
        ss_addr.sin_port = htons(ss_port);
        inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr);

        if (connect(ss_fd, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
            close(ss_fd);
            ss_fd = -1;
            pthread_mutex_lock(&ss_mutex);
            continue;
        }

        char ss_request[512];
        int len = snprintf(ss_request, sizeof(ss_request),
                           "{\"cmd\":\"CREATE\",\"filename\":\"%s\",\"content\":\"\"}\n",
                           filename);
        send(ss_fd, ss_request, len, 0);

        int bytes = recv(ss_fd, ss_response, sizeof(ss_response) - 1, 0);
        close(ss_fd);
        ss_fd = -1;

        if (bytes <= 0) {
            pthread_mutex_lock(&ss_mutex);
            continue;
        }

        ss_response[bytes] = '\0';

        if (strstr(ss_response, "\"status\":\"ERR\"")) {
            pthread_mutex_lock(&ss_mutex);
            continue;
        }

        next_ss_index = (ss_index + 1) % ss_count;
        success = 1;
        pthread_mutex_lock(&ss_mutex);
        break;
    }

    pthread_mutex_unlock(&ss_mutex);

    if (!success) {
        if (strlen(ss_response) > 0 && strstr(ss_response, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(ss_response, "reason", reason, sizeof(reason));
            if (strlen(reason) == 0) {
                strcpy(reason, "ALL_SS_FAILED");
            }
            char err[256];
            snprintf(err, sizeof(err), "{\"status\":\"ERR\",\"reason\":\"%s\"}", reason);
            send_response(client_fd, err);
        } else {
            send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"ALL_SS_DOWN\"}");
        }
        return;
    }

    FileMetadata *file = malloc(sizeof(FileMetadata));
    if (!file) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"UNKNOWN\"}");
        return;
    }

    memset(file, 0, sizeof(*file));
    strncpy(file->filename, filename, sizeof(file->filename) - 1);
    file->filename[sizeof(file->filename) - 1] = '\0';
    strncpy(file->owner, username, sizeof(file->owner) - 1);
    file->owner[sizeof(file->owner) - 1] = '\0';
    strncpy(file->ss_ip, ss_ip, sizeof(file->ss_ip) - 1);
    file->ss_ip[sizeof(file->ss_ip) - 1] = '\0';
    file->ss_port = ss_port;
    file->backup_ss_ip[0] = '\0';
    file->backup_ss_port = 0;
    file->access_list = NULL;
    file->active = 1;
    file->created_at = time(NULL);
    file->last_modified = file->created_at;
    file->last_accessed = file->created_at;
    strncpy(file->last_accessed_by, username, sizeof(file->last_accessed_by) - 1);
    file->last_accessed_by[sizeof(file->last_accessed_by) - 1] = '\0';
    file->words = 0;
    file->chars = 0;
    file->bytes = 0;

    pthread_mutex_lock(&ss_mutex);
    char debug_log[512];
    snprintf(debug_log, sizeof(debug_log), "Backup attempt: ss_count=%d for file %s", ss_count, filename);
    log_message("INFO", debug_log, "0.0.0.0", 0, "system");

    if (ss_count > 1) {
        for (int i = 0; i < ss_count; i++) {
            snprintf(debug_log, sizeof(debug_log),
                     "Checking SS[%d]: active=%d, ip=%s:%d vs primary %s:%d",
                     i, storage_servers[i].active, storage_servers[i].ip,
                     storage_servers[i].client_port, ss_ip, ss_port);
            log_message("INFO", debug_log, "0.0.0.0", 0, "system");

            if (storage_servers[i].active &&
                (strcmp(storage_servers[i].ip, ss_ip) != 0 || storage_servers[i].client_port != ss_port)) {

                char backup_ip[INET_ADDRSTRLEN];
                int backup_port = storage_servers[i].client_port;
                strncpy(backup_ip, storage_servers[i].ip, sizeof(backup_ip) - 1);
                backup_ip[sizeof(backup_ip) - 1] = '\0';

                snprintf(debug_log, sizeof(debug_log),
                         "Attempting backup creation on %s:%d", backup_ip, backup_port);
                log_message("INFO", debug_log, "0.0.0.0", 0, "system");
                pthread_mutex_unlock(&ss_mutex);

                int backup_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (backup_fd >= 0) {
                    struct sockaddr_in backup_addr;
                    backup_addr.sin_family = AF_INET;
                    backup_addr.sin_port = htons(backup_port);
                    inet_pton(AF_INET, backup_ip, &backup_addr.sin_addr);

                    if (connect(backup_fd, (struct sockaddr *)&backup_addr, sizeof(backup_addr)) == 0) {
                        char backup_request[512];
                        int len = snprintf(backup_request, sizeof(backup_request),
                                           "{\"cmd\":\"CREATE\",\"filename\":\"%s\",\"content\":\"\"}\n",
                                           filename);
                        send(backup_fd, backup_request, len, 0);

                        char backup_resp[512];
                        int bytes = recv(backup_fd, backup_resp, sizeof(backup_resp) - 1, 0);
                        if (bytes > 0) {
                            backup_resp[bytes] = '\0';
                            if (strstr(backup_resp, "\"status\":\"OK\"")) {
                                strncpy(file->backup_ss_ip, backup_ip, sizeof(file->backup_ss_ip) - 1);
                                file->backup_ss_ip[sizeof(file->backup_ss_ip) - 1] = '\0';
                                file->backup_ss_port = backup_port;

                                char backup_log[512];
                                snprintf(backup_log, sizeof(backup_log),
                                         "Backup created for %s on SS %s:%d", filename, backup_ip, backup_port);
                                log_message("INFO", backup_log, "0.0.0.0", 0, "system");
                            } else if (strstr(backup_resp, "ALREADY_EXISTS")) {
                                snprintf(debug_log, sizeof(debug_log),
                                         "Backup file exists, deleting and retrying");
                                log_message("INFO", debug_log, "0.0.0.0", 0, "system");

                                char delete_req[512];
                                len = snprintf(delete_req, sizeof(delete_req),
                                               "{\"cmd\":\"DELETE\",\"filename\":\"%s\"}\n", filename);
                                send(backup_fd, delete_req, len, 0);
                                recv(backup_fd, backup_resp, sizeof(backup_resp) - 1, 0);

                                len = snprintf(backup_request, sizeof(backup_request),
                                               "{\"cmd\":\"CREATE\",\"filename\":\"%s\",\"content\":\"\"}\n",
                                               filename);
                                send(backup_fd, backup_request, len, 0);
                                bytes = recv(backup_fd, backup_resp, sizeof(backup_resp) - 1, 0);

                                if (bytes > 0) {
                                    backup_resp[bytes] = '\0';
                                    if (strstr(backup_resp, "\"status\":\"OK\"")) {
                                        strncpy(file->backup_ss_ip, backup_ip, sizeof(file->backup_ss_ip) - 1);
                                        file->backup_ss_ip[sizeof(file->backup_ss_ip) - 1] = '\0';
                                        file->backup_ss_port = backup_port;

                                        char backup_log[512];
                                        snprintf(backup_log, sizeof(backup_log),
                                                 "Backup created for %s on SS %s:%d (after cleanup)",
                                                 filename, backup_ip, backup_port);
                                        log_message("INFO", backup_log, "0.0.0.0", 0, "system");
                                    }
                                }
                            } else {
                                snprintf(debug_log, sizeof(debug_log),
                                         "Backup failed: response=%s", backup_resp);
                                log_message("INFO", debug_log, "0.0.0.0", 0, "system");
                            }
                        } else {
                            snprintf(debug_log, sizeof(debug_log),
                                     "Backup failed: no response from SS");
                            log_message("INFO", debug_log, "0.0.0.0", 0, "system");
                        }
                        close(backup_fd);
                    } else {
                        snprintf(debug_log, sizeof(debug_log),
                                 "Backup failed: cannot connect to %s:%d", backup_ip, backup_port);
                        log_message("INFO", debug_log, "0.0.0.0", 0, "system");
                        close(backup_fd);
                    }
                } else {
                    snprintf(debug_log, sizeof(debug_log),
                             "Backup failed: socket creation failed");
                    log_message("INFO", debug_log, "0.0.0.0", 0, "system");
                }
                pthread_mutex_lock(&ss_mutex);
                break;
            }
        }
    } else {
        snprintf(debug_log, sizeof(debug_log),
                 "Backup skipped: only %d storage server(s) available", ss_count);
        log_message("INFO", debug_log, "0.0.0.0", 0, "system");
    }
    pthread_mutex_unlock(&ss_mutex);

    pthread_mutex_lock(&files_mutex);
    if (lookup_file(filename)) {
        pthread_mutex_unlock(&files_mutex);
        free(file);
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"ALREADY_EXISTS\"}");
        return;
    }
    insert_file(file);
    pthread_mutex_unlock(&files_mutex);

    char response[256];
    snprintf(response, sizeof(response),
             "{\"status\":\"OK\",\"ss_ip\":\"%s\",\"ss_port\":%d}",
             ss_ip, ss_port);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "File created: %s by %s on SS %s:%d", filename, username, ss_ip, ss_port);
    log_message("INFO", log_msg, "0.0.0.0", 0, username);

    save_metadata();
    send_response(client_fd, response);
}

void handle_info(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    parse_json_string(request, "filename", filename, sizeof(filename));

    pthread_mutex_lock(&files_mutex);

    FileMetadata *file = lookup_file(filename);
    if (!file) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"FILE_NOT_FOUND\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    char access_json[1024];
    access_json[0] = '\0';
    strcat(access_json, "[");

    char temp[256];
    snprintf(temp, sizeof(temp), "{\"user\":\"%s\",\"mode\":\"RW\"}", file->owner);
    strcat(access_json, temp);

    AccessEntry *entry = file->access_list;
    while (entry) {
        snprintf(temp, sizeof(temp), ",{\"user\":\"%s\",\"mode\":\"%s\"}", entry->username, entry->mode);
        strcat(access_json, temp);
        entry = entry->next;
    }

    strcat(access_json, "]");

    char created_str[64];
    char modified_str[64];
    char accessed_str[64];
    struct tm *tm_info;

    tm_info = localtime(&file->created_at);
    strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M:%S", tm_info);

    tm_info = localtime(&file->last_modified);
    strftime(modified_str, sizeof(modified_str), "%Y-%m-%d %H:%M:%S", tm_info);

    tm_info = localtime(&file->last_accessed);
    strftime(accessed_str, sizeof(accessed_str), "%Y-%m-%d %H:%M:%S", tm_info);

    char file_owner[MAX_USERNAME];
    char file_ss_ip[INET_ADDRSTRLEN];
    int file_ss_port = file->ss_port;
    char last_accessed_by[MAX_USERNAME];

    strncpy(file_owner, file->owner, sizeof(file_owner) - 1);
    file_owner[sizeof(file_owner) - 1] = '\0';
    strncpy(file_ss_ip, file->ss_ip, sizeof(file_ss_ip) - 1);
    file_ss_ip[sizeof(file_ss_ip) - 1] = '\0';
    strncpy(last_accessed_by, file->last_accessed_by, sizeof(last_accessed_by) - 1);
    last_accessed_by[sizeof(last_accessed_by) - 1] = '\0';

    int words = file->words;
    int chars = file->chars;
    int bytes = file->bytes;

    pthread_mutex_unlock(&files_mutex);

    int temp_words = 0;
    int temp_chars = 0;
    int temp_bytes = 0;
    if (request_file_stats(file_ss_ip, file_ss_port, filename, &temp_words, &temp_chars, &temp_bytes)) {
        words = temp_words;
        chars = temp_chars;
        bytes = temp_bytes;

        pthread_mutex_lock(&files_mutex);
        FileMetadata *file_after = lookup_file(filename);
        if (file_after) {
            file_after->words = words;
            file_after->chars = chars;
            file_after->bytes = bytes;
        }
        pthread_mutex_unlock(&files_mutex);
        save_metadata();
    }

    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
             "{\"status\":\"OK\",\"filename\":\"%s\",\"owner\":\"%s\",\"created_at\":\"%s\",\"last_modified\":\"%s\",\"last_accessed\":\"%s\",\"last_accessed_by\":\"%s\",\"words\":%d,\"chars\":%d,\"bytes\":%d,\"access\":%s,\"ss_ip\":\"%s\",\"ss_port\":%d}",
             filename, file_owner, created_str, modified_str, accessed_str, last_accessed_by,
             words, chars, bytes, access_json, file_ss_ip, file_ss_port);

    log_message("INFO", "INFO command executed", "0.0.0.0", 0, username);
    send_response(client_fd, response);
}

void handle_addaccess(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    char target[MAX_USERNAME] = {0};
    char mode[3] = {0};

    parse_json_string(request, "filename", filename, sizeof(filename));
    parse_json_string(request, "target", target, sizeof(target));
    parse_json_string(request, "mode", mode, sizeof(mode));

    pthread_mutex_lock(&files_mutex);

    FileMetadata *file = lookup_file(filename);
    if (!file) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"FILE_NOT_FOUND\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    if (strcmp(file->owner, username) != 0) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"UNAUTHORIZED\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    AccessEntry *entry = file->access_list;
    while (entry) {
        if (strcmp(entry->username, target) == 0) {
            strncpy(entry->mode, mode, sizeof(entry->mode) - 1);
            entry->mode[sizeof(entry->mode) - 1] = '\0';
            send_response(client_fd, "{\"status\":\"OK\",\"msg\":\"Access updated\"}");
            pthread_mutex_unlock(&files_mutex);
            save_metadata();
            return;
        }
        entry = entry->next;
    }

    AccessEntry *new_entry = malloc(sizeof(AccessEntry));
    if (!new_entry) {
        pthread_mutex_unlock(&files_mutex);
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"UNKNOWN\"}");
        return;
    }

    strncpy(new_entry->username, target, sizeof(new_entry->username) - 1);
    new_entry->username[sizeof(new_entry->username) - 1] = '\0';
    strncpy(new_entry->mode, mode, sizeof(new_entry->mode) - 1);
    new_entry->mode[sizeof(new_entry->mode) - 1] = '\0';
    new_entry->next = file->access_list;
    file->access_list = new_entry;

    pthread_mutex_unlock(&files_mutex);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Access added: %s to %s for %s", mode, target, filename);
    log_message("INFO", log_msg, "0.0.0.0", 0, username);

    save_metadata();
    send_response(client_fd, "{\"status\":\"OK\",\"msg\":\"Access granted\"}");
}

void handle_remaccess(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    char target[MAX_USERNAME] = {0};

    parse_json_string(request, "filename", filename, sizeof(filename));
    parse_json_string(request, "target", target, sizeof(target));

    pthread_mutex_lock(&files_mutex);

    FileMetadata *file = lookup_file(filename);
    if (!file) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"FILE_NOT_FOUND\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    if (strcmp(file->owner, username) != 0) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"UNAUTHORIZED\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    AccessEntry **indirect = &file->access_list;
    while (*indirect) {
        if (strcmp((*indirect)->username, target) == 0) {
            AccessEntry *to_free = *indirect;
            *indirect = (*indirect)->next;
            free(to_free);
            send_response(client_fd, "{\"status\":\"OK\",\"msg\":\"Access removed\"}");
            pthread_mutex_unlock(&files_mutex);
            save_metadata();
            return;
        }
        indirect = &(*indirect)->next;
    }

    send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"ACCESS_NOT_FOUND\"}");
    pthread_mutex_unlock(&files_mutex);
}

void handle_file_operation(int client_fd, const char *request, const char *username) {
    char cmd[64] = {0};
    char filename[MAX_FILENAME] = {0};

    parse_json_string(request, "cmd", cmd, sizeof(cmd));
    parse_json_string(request, "filename", filename, sizeof(filename));

    pthread_mutex_lock(&files_mutex);

    FileMetadata *file = lookup_file(filename);
    if (!file) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"FILE_NOT_FOUND\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    const char *required_mode = (strcmp(cmd, "READ") == 0 || strcmp(cmd, "STREAM") == 0) ? "R" : "W";
    if (!check_access(file, username, required_mode)) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"UNAUTHORIZED\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    file->last_accessed = time(NULL);
    strncpy(file->last_accessed_by, username, sizeof(file->last_accessed_by) - 1);
    file->last_accessed_by[sizeof(file->last_accessed_by) - 1] = '\0';

    if (strcmp(cmd, "WRITE") == 0 || strcmp(cmd, "UNDO") == 0) {
        file->last_modified = time(NULL);
    }

    char *ss_ip_to_use = file->ss_ip;
    int ss_port_to_use = file->ss_port;
    int using_backup = 0;

    int primary_alive = 0;
    int test_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (test_fd >= 0) {
        struct sockaddr_in test_addr;
        test_addr.sin_family = AF_INET;
        test_addr.sin_port = htons(file->ss_port);
        inet_pton(AF_INET, file->ss_ip, &test_addr.sin_addr);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(test_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(test_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (connect(test_fd, (struct sockaddr *)&test_addr, sizeof(test_addr)) == 0) {
            primary_alive = 1;
        }
        close(test_fd);
    }

    if (!primary_alive && file->backup_ss_ip[0] != '\0' && file->backup_ss_port != 0) {
        ss_ip_to_use = file->backup_ss_ip;
        ss_port_to_use = file->backup_ss_port;
        using_backup = 1;

        char log_msg[512];
        snprintf(log_msg, sizeof(log_msg), "Primary SS down for %s, using backup %s:%d",
                 filename, ss_ip_to_use, ss_port_to_use);
        log_message("INFO", log_msg, "0.0.0.0", 0, username);
    }

    char response[256];
    snprintf(response, sizeof(response),
             "{\"status\":\"OK\",\"ss_ip\":\"%s\",\"ss_port\":%d}",
             ss_ip_to_use, ss_port_to_use);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "%s operation: %s by %s (SS: %s:%d%s)",
             cmd, filename, username, ss_ip_to_use, ss_port_to_use,
             using_backup ? " [BACKUP]" : "");
    log_message("INFO", log_msg, "0.0.0.0", 0, username);

    pthread_mutex_unlock(&files_mutex);

    save_metadata();
    send_response(client_fd, response);
}

void handle_delete(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    parse_json_string(request, "filename", filename, sizeof(filename));

    pthread_mutex_lock(&files_mutex);

    FileMetadata *file = lookup_file(filename);
    if (!file) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"FILE_NOT_FOUND\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    if (strcmp(file->owner, username) != 0) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"UNAUTHORIZED\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    file->active = 0;
    cache_remove(filename);

    char ss_ip[INET_ADDRSTRLEN];
    char backup_ss_ip[INET_ADDRSTRLEN];
    int ss_port = file->ss_port;
    int backup_ss_port = file->backup_ss_port;
    strncpy(ss_ip, file->ss_ip, sizeof(ss_ip) - 1);
    ss_ip[sizeof(ss_ip) - 1] = '\0';
    strncpy(backup_ss_ip, file->backup_ss_ip, sizeof(backup_ss_ip) - 1);
    backup_ss_ip[sizeof(backup_ss_ip) - 1] = '\0';

    pthread_mutex_unlock(&files_mutex);

    int ss_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_fd >= 0) {
        struct sockaddr_in ss_addr;
        ss_addr.sin_family = AF_INET;
        ss_addr.sin_port = htons(ss_port);
        inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr);

        if (connect(ss_fd, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == 0) {
            char ss_request[512];
            snprintf(ss_request, sizeof(ss_request),
                     "{\"cmd\":\"DELETE\",\"filename\":\"%s\"}\n",
                     filename);
            send(ss_fd, ss_request, strlen(ss_request), 0);

            char ss_response[BUFFER_SIZE];
            recv(ss_fd, ss_response, sizeof(ss_response) - 1, 0);
        }
        close(ss_fd);
    }

    if (backup_ss_ip[0] != '\0' && backup_ss_port != 0) {
        int backup_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (backup_fd >= 0) {
            struct sockaddr_in backup_addr;
            backup_addr.sin_family = AF_INET;
            backup_addr.sin_port = htons(backup_ss_port);
            inet_pton(AF_INET, backup_ss_ip, &backup_addr.sin_addr);

            if (connect(backup_fd, (struct sockaddr *)&backup_addr, sizeof(backup_addr)) == 0) {
                char backup_request[512];
                snprintf(backup_request, sizeof(backup_request),
                         "{\"cmd\":\"DELETE\",\"filename\":\"%s\"}\n",
                         filename);
                send(backup_fd, backup_request, strlen(backup_request), 0);

                char backup_response[BUFFER_SIZE];
                recv(backup_fd, backup_response, sizeof(backup_response) - 1, 0);
            }
            close(backup_fd);
        }
    }

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "File deleted: %s by %s", filename, username);
    log_message("INFO", log_msg, "0.0.0.0", 0, username);

    save_metadata();
    send_response(client_fd, "{\"status\":\"OK\",\"msg\":\"File deleted\"}");
}

void handle_exec(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    parse_json_string(request, "filename", filename, sizeof(filename));

    pthread_mutex_lock(&files_mutex);

    FileMetadata *file = lookup_file(filename);
    if (!file) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"FILE_NOT_FOUND\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    if (!check_access(file, username, "R")) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"UNAUTHORIZED\"}");
        pthread_mutex_unlock(&files_mutex);
        return;
    }

    file->last_accessed = time(NULL);
    strncpy(file->last_accessed_by, username, sizeof(file->last_accessed_by) - 1);
    file->last_accessed_by[sizeof(file->last_accessed_by) - 1] = '\0';

    char ss_ip[INET_ADDRSTRLEN];
    int ss_port = file->ss_port;
    strncpy(ss_ip, file->ss_ip, sizeof(ss_ip) - 1);
    ss_ip[sizeof(ss_ip) - 1] = '\0';

    pthread_mutex_unlock(&files_mutex);

    int ss_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_fd < 0) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"SS_CONNECTION_FAILED\"}");
        return;
    }

    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr);

    if (connect(ss_fd, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"SS_CONNECTION_FAILED\"}");
        close(ss_fd);
        return;
    }

    char ss_request[512];
    snprintf(ss_request, sizeof(ss_request),
             "{\"cmd\":\"READ\",\"username\":\"%s\",\"filename\":\"%s\"}\n",
             username, filename);
    send(ss_fd, ss_request, strlen(ss_request), 0);

    char *ss_response = malloc(BUFFER_SIZE);
    if (!ss_response) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"MEMORY_ERROR\"}");
        close(ss_fd);
        return;
    }

    int bytes_read = recv(ss_fd, ss_response, BUFFER_SIZE - 1, 0);
    close(ss_fd);

    if (bytes_read <= 0) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"SS_READ_FAILED\"}");
        free(ss_response);
        return;
    }
    ss_response[bytes_read] = '\0';

    char content[BUFFER_SIZE] = {0};
    const char *content_start = strstr(ss_response, "\"content\":\"");
    if (content_start) {
        content_start += 11;
        const char *content_end = strchr(content_start, '"');
        if (content_end) {
            int content_len = (int)(content_end - content_start);
            if (content_len < BUFFER_SIZE) {
                strncpy(content, content_start, content_len);
                content[content_len] = '\0';
            }
        }
    }

    free(ss_response);

    if (strlen(content) == 0) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"EMPTY_FILE\"}");
        return;
    }

    char unescaped_content[BUFFER_SIZE];
    int j = 0;
    for (int i = 0; i < (int)strlen(content) && j < BUFFER_SIZE - 1; i++) {
        if (content[i] == '\\' && content[i + 1] == 'n') {
            unescaped_content[j++] = '\n';
            i++;
        } else if (content[i] == '\\' && content[i + 1] == 'r') {
            unescaped_content[j++] = '\r';
            i++;
        } else if (content[i] == '\\' && content[i + 1] == 't') {
            unescaped_content[j++] = '\t';
            i++;
        } else if (content[i] == '\\' && content[i + 1] == '\\') {
            unescaped_content[j++] = '\\';
            i++;
        } else if (content[i] == '\\' && content[i + 1] == '"') {
            unescaped_content[j++] = '"';
            i++;
        } else {
            unescaped_content[j++] = content[i];
        }
    }
    unescaped_content[j] = '\0';

    char exec_output[BUFFER_SIZE * 2] = {0};
    FILE *pipe = popen(unescaped_content, "r");
    if (!pipe) {
        send_response(client_fd, "{\"status\":\"ERR\",\"reason\":\"EXEC_FAILED\"}");
        return;
    }

    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        if (strlen(exec_output) + strlen(buffer) < sizeof(exec_output) - 1) {
            strcat(exec_output, buffer);
        }
    }

    int exit_code = pclose(pipe);

    char escaped_output[BUFFER_SIZE * 2];
    int k = 0;
    for (int i = 0; i < (int)strlen(exec_output) && k < (int)sizeof(escaped_output) - 2; i++) {
        if (exec_output[i] == '"') {
            escaped_output[k++] = '\\';
            escaped_output[k++] = '"';
        } else if (exec_output[i] == '\n') {
            escaped_output[k++] = '\\';
            escaped_output[k++] = 'n';
        } else if (exec_output[i] == '\r') {
            escaped_output[k++] = '\\';
            escaped_output[k++] = 'r';
        } else if (exec_output[i] == '\t') {
            escaped_output[k++] = '\\';
            escaped_output[k++] = 't';
        } else if (exec_output[i] == '\\') {
            escaped_output[k++] = '\\';
            escaped_output[k++] = '\\';
        } else {
            escaped_output[k++] = exec_output[i];
        }
    }
    escaped_output[k] = '\0';

    char response[BUFFER_SIZE * 3];
    snprintf(response, sizeof(response),
             "{\"status\":\"OK\",\"output\":\"%s\",\"exit_code\":%d}",
             escaped_output, WEXITSTATUS(exit_code));
    send_response(client_fd, response);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "EXEC: %s by %s (exit_code=%d)",
             filename, username, WEXITSTATUS(exit_code));
    log_message("INFO", log_msg, "0.0.0.0", 0, username);

    save_metadata();
}
