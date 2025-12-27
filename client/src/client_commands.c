//AI code starts
#include "client_commands.h"
#include "client_network.h"
#include "client_utils.h"

static void print_view_response(const char *response, const char *flags);

void handle_view(const char *flags) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[256];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"VIEW\",\"username\":\"%s\",\"flags\":\"%s\"}",
             current_username, flags);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (response) {
        print_view_response(response, flags);
        free(response);
    } else {
        printf("Error: No response from server\n");
    }

    close(nm_fd);
}

void handle_list(void) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[256];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"LIST\",\"username\":\"%s\"}",
             current_username);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (response) {
        if (strstr(response, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(response, "reason", reason, sizeof(reason));
            printf("Error: %s\n", reason);
        } else {
            const char *pos = strstr(response, "\"users\":[");
            if (pos) {
                pos += 9;
                while (*pos && *pos != ']') {
                    if (*pos == '"') {
                        pos++;
                        const char *end = strchr(pos, '"');
                        if (end) {
                            char username[MAX_USERNAME];
                            int len = (int)(end - pos);
                            if (len >= MAX_USERNAME) {
                                len = MAX_USERNAME - 1;
                            }
                            strncpy(username, pos, (size_t)len);
                            username[len] = '\0';
                            printf("--> %s\n", username);
                            pos = end + 1;
                        }
                    }
                    pos++;
                }
            }
        }
        free(response);
    }

    close(nm_fd);
}

void handle_create(const char *filename) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"CREATE\",\"username\":\"%s\",\"filename\":\"%s\"}",
             current_username, filename);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (response) {
        if (strstr(response, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(response, "reason", reason, sizeof(reason));
            printf("Error: %s\n", reason);
        } else {
            printf("File Created Successfully!\n");
        }
        free(response);
    }

    close(nm_fd);
}

void handle_info(const char *filename) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"INFO\",\"username\":\"%s\",\"filename\":\"%s\"}",
             current_username, filename);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (response) {
        if (strstr(response, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(response, "reason", reason, sizeof(reason));
            printf("Error: %s\n", reason);
        } else {
            char fname[MAX_FILENAME] = {0};
            char owner[MAX_USERNAME] = {0};
            char created[64] = {0};
            char modified[64] = {0};
            char accessed[64] = {0};
            char accessed_by[MAX_USERNAME] = {0};

            parse_json_string(response, "filename", fname, sizeof(fname));
            parse_json_string(response, "owner", owner, sizeof(owner));
            parse_json_string(response, "created_at", created, sizeof(created));
            parse_json_string(response, "last_modified", modified, sizeof(modified));
            parse_json_string(response, "last_accessed", accessed, sizeof(accessed));
            parse_json_string(response, "last_accessed_by", accessed_by, sizeof(accessed_by));

            printf("--> File: %s\n", fname);
            printf("--> Owner: %s\n", owner);
            printf("--> Created: %s\n", strlen(created) > 0 ? created : "N/A");
            printf("--> Last Modified: %s\n", strlen(modified) > 0 ? modified : "N/A");
            int stat_words = parse_json_int(response, "words");
            int stat_chars = parse_json_int(response, "chars");
            int stat_bytes = parse_json_int(response, "bytes");

            printf("--> Words: %d\n", stat_words);
            printf("--> Characters: %d\n", stat_chars);
            printf("--> Size: %d bytes\n", stat_bytes);
            printf("--> Access:\n");
            printf("    %s (RW)\n", owner);

            const char *access_section = strstr(response, "\"access\"");
            if (access_section) {
                const char *cursor = strchr(access_section, '[');
                if (cursor) {
                    cursor++;
                    while (*cursor) {
                        while (*cursor && (isspace((unsigned char)*cursor) || *cursor == ',')) {
                            cursor++;
                        }
                        if (*cursor == ']' || *cursor == '\0') {
                            break;
                        }
                        if (*cursor != '{') {
                            cursor++;
                            continue;
                        }

                        cursor++;
                        int depth = 1;
                        const char *obj_start = cursor;
                        const char *obj_end = obj_start;
                        while (*obj_end && depth > 0) {
                            if (*obj_end == '{') {
                                depth++;
                            } else if (*obj_end == '}') {
                                depth--;
                            }
                            obj_end++;
                        }
                        if (depth != 0) {
                            break;
                        }

                        size_t obj_len = (size_t)((obj_end - 1) - obj_start);
                        if (obj_len >= 256) {
                            obj_len = 255;
                        }
                        char access_obj[256];
                        memcpy(access_obj, obj_start, obj_len);
                        access_obj[obj_len] = '\0';

                        char shared_user[MAX_USERNAME] = {0};
                        char shared_mode[8] = {0};
                        parse_json_string(access_obj, "user", shared_user, sizeof(shared_user));
                        parse_json_string(access_obj, "mode", shared_mode, sizeof(shared_mode));

                        if (shared_user[0] != '\0' && strcmp(shared_user, owner) != 0) {
                            const char *mode_to_print = (shared_mode[0] != '\0') ? shared_mode : "R";
                            printf("    %s (%s)\n", shared_user, mode_to_print);
                        }

                        cursor = obj_end;
                    }
                }
            }

            printf("--> Last Accessed: %s by %s\n",
                   strlen(accessed) > 0 ? accessed : "N/A",
                   strlen(accessed_by) > 0 ? accessed_by : owner);
        }
        free(response);
    }

    close(nm_fd);
}

void handle_addaccess(const char *filename, const char *target, const char *mode) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"ADDACCESS\",\"username\":\"%s\",\"filename\":\"%s\",\"target\":\"%s\",\"mode\":\"%s\"}",
             current_username, filename, target, mode);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (response) {
        if (strstr(response, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(response, "reason", reason, sizeof(reason));
            printf("Error: %s\n", reason);
        } else {
            printf("Access granted successfully!\n");
        }
        free(response);
    }

    close(nm_fd);
}

void handle_remaccess(const char *filename, const char *target) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"REMACCESS\",\"username\":\"%s\",\"filename\":\"%s\",\"target\":\"%s\"}",
             current_username, filename, target);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (response) {
        if (strstr(response, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(response, "reason", reason, sizeof(reason));
            printf("Error: %s\n", reason);
        } else {
            printf("Access removed successfully!\n");
        }
        free(response);
    }

    close(nm_fd);
}

void handle_read(const char *filename) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"READ\",\"username\":\"%s\",\"filename\":\"%s\"}",
             current_username, filename);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (!response) {
        close(nm_fd);
        return;
    }

    if (strstr(response, "\"status\":\"ERR\"")) {
        char reason[128] = {0};
        parse_json_string(response, "reason", reason, sizeof(reason));
        printf("Error: %s\n", reason);
        free(response);
        close(nm_fd);
        return;
    }

    char ss_ip[INET_ADDRSTRLEN] = {0};
    int ss_port = 0;
    parse_json_string(response, "ss_ip", ss_ip, sizeof(ss_ip));
    ss_port = parse_json_int(response, "ss_port");

    free(response);
    close(nm_fd);

    int ss_fd = connect_to_ss(ss_ip, ss_port);
    if (ss_fd < 0) {
        printf("Error: Could not connect to Storage Server\n");
        return;
    }

    char ss_request[512];
    snprintf(ss_request, sizeof(ss_request),
             "{\"cmd\":\"READ\",\"username\":\"%s\",\"filename\":\"%s\"}",
             current_username, filename);

    send_message(ss_fd, ss_request);
    char *ss_response = receive_message(ss_fd);

    if (ss_response) {
        if (strstr(ss_response, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(ss_response, "reason", reason, sizeof(reason));
            printf("Error: %s\n", reason);
        } else {
            char content[BUFFER_SIZE] = {0};
            parse_json_string(ss_response, "content", content, sizeof(content));
            printf("%s\n", content);
        }
        free(ss_response);
    }

    close(ss_fd);
}

void handle_write(const char *filename, int sentence_index) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"WRITE\",\"username\":\"%s\",\"filename\":\"%s\"}",
             current_username, filename);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (!response) {
        close(nm_fd);
        return;
    }

    if (strstr(response, "\"status\":\"ERR\"")) {
        char reason[128] = {0};
        parse_json_string(response, "reason", reason, sizeof(reason));
        printf("Error: %s\n", reason);
        free(response);
        close(nm_fd);
        return;
    }

    char ss_ip[INET_ADDRSTRLEN] = {0};
    int ss_port = 0;
    parse_json_string(response, "ss_ip", ss_ip, sizeof(ss_ip));
    ss_port = parse_json_int(response, "ss_port");

    free(response);
    close(nm_fd);

    int ss_fd = connect_to_ss(ss_ip, ss_port);
    if (ss_fd < 0) {
        printf("Error: Could not connect to Storage Server\n");
        return;
    }

    char ss_request[512];
    snprintf(ss_request, sizeof(ss_request),
             "{\"cmd\":\"WRITE\",\"username\":\"%s\",\"filename\":\"%s\",\"sentence_index\":%d}",
             current_username, filename, sentence_index);

    send_message(ss_fd, ss_request);
    char *ss_response = receive_message(ss_fd);

    if (!ss_response || strstr(ss_response, "\"status\":\"ERR\"")) {
        if (ss_response) {
            char reason[128] = {0};
            parse_json_string(ss_response, "reason", reason, sizeof(reason));
            printf("Error: %s\n", reason);
            free(ss_response);
        }
        close(ss_fd);
        return;
    }

    free(ss_response);

    printf("Client: ");
    char input[1024];
    while (fgets(input, sizeof(input), stdin)) {
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "ETIRW") == 0) {
            send_message(ss_fd, "{\"cmd\":\"ETIRW\"}");
            ss_response = receive_message(ss_fd);
            if (ss_response) {
                if (strstr(ss_response, "\"status\":\"OK\"")) {
                    printf("Write Successful!\n");
                } else {
                    printf("Write Failed: %s\n", ss_response);
                }
                free(ss_response);
            } else {
                printf("Write Failed: No response from server\n");
            }
            break;
        }

        int word_index;
        char content[512];
        char index_token[32];

        const char *space_pos = strchr(input, ' ');
        if (!space_pos || space_pos == input) {
            printf("Invalid input. Use '<index>[!] <words>'.\n");
            printf("Client: ");
            continue;
        }

        size_t idx_len = (size_t)(space_pos - input);
        if (idx_len >= sizeof(index_token)) {
            printf("Invalid input. Use '<index>[!] <words>'.\n");
            printf("Client: ");
            continue;
        }
        memcpy(index_token, input, idx_len);
        index_token[idx_len] = '\0';

        const char *content_start = space_pos + 1;
        strncpy(content, content_start, sizeof(content) - 1);
        content[sizeof(content) - 1] = '\0';

        int replace_word = 0;
        size_t token_len = strlen(index_token);

        int all_digits_before = 1;
        for (size_t i = 0; i + 1 < token_len; i++) {
            if (!isdigit((unsigned char)index_token[i])) {
                all_digits_before = 0;
                break;
            }
        }

        if (token_len > 1 && index_token[token_len - 1] == '!' && all_digits_before) {
            replace_word = 1;
            index_token[token_len - 1] = '\0';
            token_len--;
        }

        if (token_len == 0) {
            printf("Invalid input. Use '<index>[!] <words>'.\n");
            printf("Client: ");
            continue;
        }

        int valid_index = 1;
        for (size_t i = 0; i < token_len; i++) {
            if (!isdigit((unsigned char)index_token[i])) {
                valid_index = 0;
                break;
            }
        }

        if (!valid_index) {
            printf("Invalid input. Use '<index>[!] <words>'.\n");
            printf("Client: ");
            continue;
        }

        word_index = atoi(index_token);

        char escaped_content[1024];
        size_t j = 0;
        for (size_t i = 0; i < strlen(content) && j < sizeof(escaped_content) - 2; i++) {
            char ch = content[i];
            if (ch == '\\') {
                escaped_content[j++] = '\\';
                escaped_content[j++] = '\\';
            } else if (ch == '"') {
                escaped_content[j++] = '\\';
                escaped_content[j++] = '"';
            } else if (ch == '\n') {
                escaped_content[j++] = '\\';
                escaped_content[j++] = 'n';
            } else if (ch == '\r') {
                escaped_content[j++] = '\\';
                escaped_content[j++] = 'r';
            } else if (ch == '\t') {
                escaped_content[j++] = '\\';
                escaped_content[j++] = 't';
            } else {
                escaped_content[j++] = ch;
            }
        }
        escaped_content[j] = '\0';

        char update[2048];
        if (replace_word) {
            snprintf(update, sizeof(update),
                     "{\"cmd\":\"UPDATE\",\"word_index\":%d,\"content\":\"%s\",\"mode\":\"replace\"}",
                     word_index, escaped_content);
        } else {
            snprintf(update, sizeof(update),
                     "{\"cmd\":\"UPDATE\",\"word_index\":%d,\"content\":\"%s\"}",
                     word_index, escaped_content);
        }

        send_message(ss_fd, update);
        ss_response = receive_message(ss_fd);
        if (ss_response) {
            if (strstr(ss_response, "\"status\":\"ERR\"")) {
                char reason[128] = {0};
                parse_json_string(ss_response, "reason", reason, sizeof(reason));
                printf("Update Failed: %s\n", reason);
            } else {
                printf("Update acknowledged.\n");
            }
            free(ss_response);
        } else {
            printf("Update Failed: No response from server\n");
            break;
        }

        printf("Client: ");
    }

    close(ss_fd);
}

void handle_stream(const char *filename) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"STREAM\",\"username\":\"%s\",\"filename\":\"%s\"}",
             current_username, filename);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (!response) {
        close(nm_fd);
        return;
    }

    if (strstr(response, "\"status\":\"ERR\"")) {
        char reason[128] = {0};
        parse_json_string(response, "reason", reason, sizeof(reason));
        printf("Error: %s\n", reason);
        free(response);
        close(nm_fd);
        return;
    }

    char ss_ip[INET_ADDRSTRLEN] = {0};
    int ss_port = 0;
    parse_json_string(response, "ss_ip", ss_ip, sizeof(ss_ip));
    ss_port = parse_json_int(response, "ss_port");

    free(response);
    close(nm_fd);

    int ss_fd = connect_to_ss(ss_ip, ss_port);
    if (ss_fd < 0) {
        printf("Error: Could not connect to Storage Server\n");
        return;
    }

    char ss_request[512];
    snprintf(ss_request, sizeof(ss_request),
             "{\"cmd\":\"STREAM\",\"filename\":\"%s\"}",
             filename);

    send_message(ss_fd, ss_request);

    char buffer[256];
    char word[256];
    size_t word_len = 0;
    int stop_received = 0;
    int server_error = 0;
    int printed_words = 0;

    while (1) {
        int bytes_read = recv(ss_fd, buffer, sizeof(buffer), 0);
        if (bytes_read <= 0) {
            break;
        }

        for (int i = 0; i < bytes_read; i++) {
            char ch = buffer[i];
            if (ch == '\0' || ch == '\n') {
                if (word_len == 0) {
                    continue;
                }

                word[word_len] = '\0';
                word_len = 0;

                if (strcmp(word, "STOP") == 0) {
                    stop_received = 1;
                    break;
                }

                if (word[0] == '{') {
                    if (strstr(word, "\"status\":\"ERR\"")) {
                        char reason[128] = {0};
                        parse_json_string(word, "reason", reason, sizeof(reason));
                        if (strlen(reason) == 0) {
                            strcpy(reason, "STREAM_FAILED");
                        }
                        printf("Error: %s\n", reason);
                    }
                    server_error = 1;
                    break;
                }

                printf("%s ", word);
                fflush(stdout);
                usleep(100000);
                printed_words = 1;
            } else if ((unsigned char)ch >= 32 || ch == '\t') {
                if (word_len < sizeof(word) - 1) {
                    word[word_len++] = ch;
                }
            }
        }

        if (stop_received || server_error) {
            break;
        }
    }

    if (!server_error && !stop_received && word_len > 0) {
        word[word_len] = '\0';
        if (strcmp(word, "STOP") == 0) {
            stop_received = 1;
        } else if (word[0] == '{' && strstr(word, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(word, "reason", reason, sizeof(reason));
            if (strlen(reason) == 0) {
                strcpy(reason, "STREAM_FAILED");
            }
            printf("Error: %s\n", reason);
            server_error = 1;
        } else if (word[0] != '\0') {
            printf("%s ", word);
            fflush(stdout);
            usleep(100000);
            printed_words = 1;
        }
    }

    if (!server_error) {
        if (stop_received) {
            printf("\n");
        } else {
            if (printed_words) {
                printf("\n");
            }
            printf("Error: storage server disconnected mid-stream.\n");
        }
    }

    close(ss_fd);
}

void handle_undo(const char *filename) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"UNDO\",\"username\":\"%s\",\"filename\":\"%s\"}",
             current_username, filename);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (!response) {
        close(nm_fd);
        return;
    }

    if (strstr(response, "\"status\":\"ERR\"")) {
        char reason[128] = {0};
        parse_json_string(response, "reason", reason, sizeof(reason));
        printf("Error: %s\n", reason);
        free(response);
        close(nm_fd);
        return;
    }

    char ss_ip[INET_ADDRSTRLEN] = {0};
    int ss_port = 0;
    parse_json_string(response, "ss_ip", ss_ip, sizeof(ss_ip));
    ss_port = parse_json_int(response, "ss_port");

    free(response);
    close(nm_fd);

    int ss_fd = connect_to_ss(ss_ip, ss_port);
    if (ss_fd < 0) {
        printf("Error: Could not connect to Storage Server\n");
        return;
    }

    char ss_request[512];
    snprintf(ss_request, sizeof(ss_request),
             "{\"cmd\":\"UNDO\",\"filename\":\"%s\"}",
             filename);

    send_message(ss_fd, ss_request);
    char *ss_response = receive_message(ss_fd);

    if (ss_response) {
        if (strstr(ss_response, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(ss_response, "reason", reason, sizeof(reason));
            printf("Error: %s\n", reason);
        } else {
            printf("Undo Successful!\n");
        }
        free(ss_response);
    }

    close(ss_fd);
}

void handle_delete(const char *filename) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"DELETE\",\"username\":\"%s\",\"filename\":\"%s\"}",
             current_username, filename);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (response) {
        if (strstr(response, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(response, "reason", reason, sizeof(reason));
            printf("Error: %s\n", reason);
        } else {
            printf("File '%s' deleted successfully!\n", filename);
        }
        free(response);
    }

    close(nm_fd);
}

void handle_exec(const char *filename) {
    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        return;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "{\"cmd\":\"EXEC\",\"username\":\"%s\",\"filename\":\"%s\"}",
             current_username, filename);

    send_message(nm_fd, request);
    char *response = receive_message(nm_fd);

    if (response) {
        if (strstr(response, "\"status\":\"ERR\"")) {
            char reason[128] = {0};
            parse_json_string(response, "reason", reason, sizeof(reason));
            printf("Error: %s\n", reason);
        } else {
            char output[BUFFER_SIZE] = {0};
            parse_json_string(response, "output", output, sizeof(output));

            char unescaped[BUFFER_SIZE];
            int j = 0;
            for (int i = 0; i < (int)strlen(output) && j < BUFFER_SIZE - 1; i++) {
                if (output[i] == '\\' && output[i + 1] == 'n') {
                    unescaped[j++] = '\n';
                    i++;
                } else if (output[i] == '\\' && output[i + 1] == 'r') {
                    unescaped[j++] = '\r';
                    i++;
                } else if (output[i] == '\\' && output[i + 1] == 't') {
                    unescaped[j++] = '\t';
                    i++;
                } else if (output[i] == '\\' && output[i + 1] == '\\') {
                    unescaped[j++] = '\\';
                    i++;
                } else if (output[i] == '\\' && output[i + 1] == '"') {
                    unescaped[j++] = '"';
                    i++;
                } else {
                    unescaped[j++] = output[i];
                }
            }
            unescaped[j] = '\0';

            printf("%s", unescaped);
            if (j > 0 && unescaped[j - 1] != '\n') {
                printf("\n");
            }
        }
        free(response);
    }

    close(nm_fd);
}

void print_help(void) {
    printf("\n=== Available Commands ===\n\n");
    printf("File Operations:\n");
    printf("  VIEW [-a] [-l] [-al]     - List files (use -a for all, -l for details)\n");
    printf("  CREATE <filename>        - Create a new file\n");
    printf("  READ <filename>          - Display file content\n");
    printf("  WRITE <filename> <sent#> - Edit a sentence in the file\n");
    printf("  DELETE <filename>        - Delete a file\n");
    printf("  INFO <filename>          - Show file metadata\n");
    printf("  STREAM <filename>        - Stream file content word-by-word\n");
    printf("  UNDO <filename>          - Undo last change to file\n");
    printf("  EXEC <filename>          - Execute file as shell commands\n\n");
    printf("Access Control:\n");
    printf("  ADDACCESS -R <file> <user>  - Grant read access\n");
    printf("  ADDACCESS -W <file> <user>  - Grant write access\n");
    printf("  REMACCESS <file> <user>     - Remove access\n\n");
    printf("Other:\n");
    printf("  LIST                     - List all users\n");
    printf("  help                     - Show this help\n");
    printf("  exit/quit                - Exit client\n\n");
}

static void print_view_response(const char *response, const char *flags) {
    if (strstr(response, "\"status\":\"ERR\"")) {
        char reason[128] = {0};
        parse_json_string(response, "reason", reason, sizeof(reason));
        printf("Error: %s\n", reason);
        return;
    }

    int show_details = (strstr(flags, "l") != NULL);

    if (show_details) {
        printf("-------------------------------------------------------------------------------\n");
        printf("|  Filename  | Words | Chars | Bytes | Last Access Time    | Owner       |\n");
        printf("|------------|-------|-------|-------|---------------------|-------------|\n");

        const char *files_start = strstr(response, "\"files\":[");
        if (files_start) {
            const char *cursor = strchr(files_start, '[');
            if (cursor) {
                cursor++;

                while (*cursor) {
                    while (*cursor && (isspace((unsigned char)*cursor) || *cursor == ',')) {
                        cursor++;
                    }

                    if (*cursor == ']' || *cursor == '\0') {
                        break;
                    }

                    if (*cursor == '{') {
                        cursor++;

                        int depth = 1;
                        const char *obj_start = cursor;
                        const char *obj_end = cursor;
                        while (*obj_end && depth > 0) {
                            if (*obj_end == '{') {
                                depth++;
                            } else if (*obj_end == '}') {
                                depth--;
                            }
                            obj_end++;
                        }

                        if (depth != 0) {
                            break;
                        }

                        size_t obj_len = (size_t)((obj_end - 1) - obj_start);
                        if (obj_len >= 512) {
                            obj_len = 511;
                        }
                        char file_obj[512];
                        memcpy(file_obj, obj_start, obj_len);
                        file_obj[obj_len] = '\0';

                        char filename[MAX_FILENAME] = {0};
                        char owner[MAX_USERNAME] = {0};
                        char last_accessed[64] = {0};
                        int words = 0;
                        int chars = 0;
                        int bytes = 0;

                        parse_json_string(file_obj, "filename", filename, sizeof(filename));
                        parse_json_string(file_obj, "owner", owner, sizeof(owner));
                        parse_json_string(file_obj, "last_accessed", last_accessed, sizeof(last_accessed));
                        words = parse_json_int(file_obj, "words");
                        chars = parse_json_int(file_obj, "chars");
                        bytes = parse_json_int(file_obj, "bytes");

                        if (strlen(filename) > 0) {
                            const char *access_display = strlen(last_accessed) > 0 ? last_accessed : "N/A";
                            printf("| %-10.10s | %5d | %5d | %5d | %-19.19s | %-11.11s |\n",
                                   filename, words, chars, bytes, access_display, owner);
                        }

                        cursor = obj_end;
                    } else {
                        cursor++;
                    }
                }
            }
        }
        printf("-------------------------------------------------------------------------------\n");
    } else {
        const char *pos = strstr(response, "\"files\":[");
        if (pos) {
            pos += 9;
            while (*pos && *pos != ']') {
                if (*pos == '"') {
                    pos++;
                    const char *end = strchr(pos, '"');
                    if (end) {
                        char filename[MAX_FILENAME];
                        int len = (int)(end - pos);
                        if (len >= MAX_FILENAME) {
                            len = MAX_FILENAME - 1;
                        }
                        strncpy(filename, pos, (size_t)len);
                        filename[len] = '\0';
                        printf("--> %s\n", filename);
                        pos = end + 1;
                    }
                }
                pos++;
            }
        }
    }
}
//AI code ends