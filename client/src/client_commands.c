//AI code starts
#include "client_commands.h"
#include "client_network.h"
#include "client_utils.h"

/* Where the name server sent us, plus the ticket that storage server needs. */
typedef struct {
    char ip[INET_ADDRSTRLEN];
    int port;
    char user[MAX_USERNAME];
    long expires;
    char ticket[DNFS_HEX_LEN + 1];
} Route;

static int valid_name_or_warn(const char *filename) {
    if (!dnfs_valid_filename(filename)) {
        printf("Error: INVALID_FILENAME (use letters, digits, '.', '_' or '-', not starting with '.')\n");
        return 0;
    }
    return 1;
}

/* Builds ",\"filename\":\"...\"" plus any extra fields. */
static char *filename_fields(const char *filename, const char *extra) {
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, ",\"filename\":");
    sb_append_json(&sb, filename);
    sb_append(&sb, extra);
    return sb_take(&sb);
}

/* Runs a name server command that only needs an OK/error reply. */
static void simple_nm_command(const char *cmd, const char *fields, const char *success) {
    char *response = nm_call(cmd, fields);
    if (!response) {
        return;
    }
    if (response_is_ok(response)) {
        printf("%s\n", success);
    } else {
        print_error(response, "Error");
    }
    free(response);
}

/* Asks the name server which storage server to use for cmd on filename. */
static int get_route(const char *cmd, const char *filename, Route *route) {
    char *fields = filename_fields(filename, NULL);
    char *response = fields ? nm_call(cmd, fields) : NULL;
    free(fields);
    if (!response) {
        return 0;
    }
    if (!response_is_ok(response)) {
        print_error(response, "Error");
        free(response);
        return 0;
    }
    parse_json_string(response, "ss_ip", route->ip, sizeof(route->ip));
    route->port = parse_json_int(response, "ss_port");
    parse_json_string(response, "user", route->user, sizeof(route->user));
    parse_json_string(response, "ticket", route->ticket, sizeof(route->ticket));
    route->expires = 0;
    json_get_long(response, "expires", &route->expires);
    free(response);
    return 1;
}

static int open_route(const Route *route, Conn *conn) {
    if (!conn_open(conn, route->ip, route->port)) {
        printf("Error: Could not connect to Storage Server %s:%d\n", route->ip, route->port);
        return 0;
    }
    return 1;
}

/* Sends cmd with the ticket on an open storage server connection. */
static int send_ss_command(Conn *conn, const char *cmd, const char *filename, const Route *route,
                           const char *extra) {
    StrBuf sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"cmd\":\"%s\",\"filename\":", cmd);
    sb_append_json(&sb, filename);
    sb_append(&sb, ",\"user\":");
    sb_append_json(&sb, route->user);
    sb_appendf(&sb, ",\"expires\":%ld,\"ticket\":\"%s\"", route->expires, route->ticket);
    sb_append(&sb, extra);
    sb_append(&sb, "}");
    char *request = sb_take(&sb);
    int ok = request && conn_send(conn, request);
    free(request);
    return ok;
}

/* Iterates over the elements of a JSON array value; returns the start of
 * the next element and sets *end past it, or NULL at the end. */
static const char *next_element(const char **cursor, const char **end) {
    const char *p = json_skip_ws(*cursor);
    if (*p == ',') {
        p = json_skip_ws(p + 1);
    }
    if (*p == ']' || !*p) {
        return NULL;
    }
    *end = json_skip_value(p);
    if (!*end) {
        return NULL;
    }
    *cursor = *end;
    return p;
}

static char *dup_range(const char *start, const char *end) {
    return strndup(start, (size_t)(end - start));
}

void handle_view(const char *flags) {
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, ",\"flags\":");
    sb_append_json(&sb, flags);
    char *fields = sb_take(&sb);
    char *response = fields ? nm_call("VIEW", fields) : NULL;
    free(fields);
    if (!response) {
        return;
    }
    if (!response_is_ok(response)) {
        print_error(response, "Error");
        free(response);
        return;
    }

    int show_details = strchr(flags, 'l') != NULL;
    const char *files = json_find_value(response, "files");
    if (show_details) {
        printf("----------------------------------------------------------------------------------------\n");
        printf("| %-20s | %6s | %6s | %7s | %-16s | %-12s |\n",
               "Filename", "Words", "Chars", "Bytes", "Last Access", "Owner");
        printf("|----------------------|--------|--------|---------|------------------|--------------|\n");
    }
    if (files && *files == '[') {
        const char *cursor = files + 1;
        const char *end;
        const char *elem;
        while ((elem = next_element(&cursor, &end)) != NULL) {
            if (show_details && *elem == '{') {
                char *obj = dup_range(elem, end);
                char filename[MAX_FILENAME] = {0};
                char owner[MAX_USERNAME] = {0};
                char accessed[32] = {0};
                parse_json_string(obj, "filename", filename, sizeof(filename));
                parse_json_string(obj, "owner", owner, sizeof(owner));
                parse_json_string(obj, "last_accessed", accessed, sizeof(accessed));
                printf("| %-20.20s | %6d | %6d | %7d | %-16.16s | %-12.12s |\n",
                       filename, parse_json_int(obj, "words"), parse_json_int(obj, "chars"),
                       parse_json_int(obj, "bytes"), accessed[0] ? accessed : "N/A", owner);
                free(obj);
            } else if (*elem == '"') {
                char *name = json_read_string(elem, NULL);
                if (name) {
                    printf("--> %s\n", name);
                }
                free(name);
            }
        }
    }
    if (show_details) {
        printf("----------------------------------------------------------------------------------------\n");
    }
    free(response);
}

void handle_list(void) {
    char *response = nm_call("LIST", NULL);
    if (!response) {
        return;
    }
    if (!response_is_ok(response)) {
        print_error(response, "Error");
        free(response);
        return;
    }
    const char *users = json_find_value(response, "users");
    if (users && *users == '[') {
        const char *cursor = users + 1;
        const char *end;
        const char *elem;
        while ((elem = next_element(&cursor, &end)) != NULL) {
            char *name = *elem == '"' ? json_read_string(elem, NULL) : NULL;
            if (name) {
                printf("--> %s\n", name);
            }
            free(name);
        }
    }
    free(response);
}

void handle_create(const char *filename) {
    if (!valid_name_or_warn(filename)) {
        return;
    }
    char *fields = filename_fields(filename, NULL);
    if (fields) {
        simple_nm_command("CREATE", fields, "File Created Successfully!");
    }
    free(fields);
}

void handle_info(const char *filename) {
    if (!valid_name_or_warn(filename)) {
        return;
    }
    char *fields = filename_fields(filename, NULL);
    char *response = fields ? nm_call("INFO", fields) : NULL;
    free(fields);
    if (!response) {
        return;
    }
    if (!response_is_ok(response)) {
        print_error(response, "Error");
        free(response);
        return;
    }

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
    printf("--> Created: %s\n", created[0] ? created : "N/A");
    printf("--> Last Modified: %s\n", modified[0] ? modified : "N/A");
    printf("--> Words: %d\n", parse_json_int(response, "words"));
    printf("--> Characters: %d\n", parse_json_int(response, "chars"));
    printf("--> Size: %d bytes\n", parse_json_int(response, "bytes"));
    printf("--> Access:\n");

    const char *access = json_find_value(response, "access");
    if (access && *access == '[') {
        const char *cursor = access + 1;
        const char *end;
        const char *elem;
        while ((elem = next_element(&cursor, &end)) != NULL) {
            char *obj = dup_range(elem, end);
            char user[MAX_USERNAME] = {0};
            char mode[8] = {0};
            parse_json_string(obj, "user", user, sizeof(user));
            parse_json_string(obj, "mode", mode, sizeof(mode));
            if (user[0]) {
                printf("    %s (%s)\n", user, mode[0] ? mode : "R");
            }
            free(obj);
        }
    }

    printf("--> Last Accessed: %s by %s\n", accessed[0] ? accessed : "N/A",
           accessed_by[0] ? accessed_by : owner);
    free(response);
}

void handle_addaccess(const char *filename, const char *target, const char *mode) {
    if (!valid_name_or_warn(filename)) {
        return;
    }
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, ",\"filename\":");
    sb_append_json(&sb, filename);
    sb_append(&sb, ",\"target\":");
    sb_append_json(&sb, target);
    sb_append(&sb, ",\"mode\":");
    sb_append_json(&sb, mode);
    char *fields = sb_take(&sb);
    if (fields) {
        simple_nm_command("ADDACCESS", fields, "Access granted successfully!");
    }
    free(fields);
}

void handle_remaccess(const char *filename, const char *target) {
    if (!valid_name_or_warn(filename)) {
        return;
    }
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, ",\"filename\":");
    sb_append_json(&sb, filename);
    sb_append(&sb, ",\"target\":");
    sb_append_json(&sb, target);
    char *fields = sb_take(&sb);
    if (fields) {
        simple_nm_command("REMACCESS", fields, "Access removed successfully!");
    }
    free(fields);
}

void handle_read(const char *filename) {
    Route route;
    Conn conn;
    if (!valid_name_or_warn(filename) || !get_route("READ", filename, &route) || !open_route(&route, &conn)) {
        return;
    }

    char *response = send_ss_command(&conn, "READ", filename, &route, NULL) ? conn_recv(&conn) : NULL;
    if (!response) {
        printf("Error: No response from Storage Server\n");
    } else if (!response_is_ok(response)) {
        print_error(response, "Error");
    } else {
        char *content = json_get_str_alloc(response, "content");
        printf("%s\n", content ? content : "");
        free(content);
    }
    free(response);
    conn_close(&conn);
}

/* Parses "<index>[!] <words>" into the index, replace flag and text. */
static int parse_update_line(const char *line, int *word_index, int *replace, const char **text) {
    const char *space = strchr(line, ' ');
    if (!space || space == line || space - line >= 16) {
        return 0;
    }
    char token[16];
    size_t len = (size_t)(space - line);
    memcpy(token, line, len);
    token[len] = '\0';

    *replace = 0;
    if (len > 1 && token[len - 1] == '!') {
        *replace = 1;
        token[--len] = '\0';
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)token[i])) {
            return 0;
        }
    }
    *word_index = atoi(token);
    *text = space + 1;
    return 1;
}

void handle_write(const char *filename, int sentence_index) {
    Route route;
    Conn conn;
    if (!valid_name_or_warn(filename) || !get_route("WRITE", filename, &route) || !open_route(&route, &conn)) {
        return;
    }

    char extra[48];
    snprintf(extra, sizeof(extra), ",\"sentence_index\":%d", sentence_index);
    char *response = send_ss_command(&conn, "WRITE", filename, &route, extra) ? conn_recv(&conn) : NULL;
    if (!response || !response_is_ok(response)) {
        if (response) {
            print_error(response, "Error");
        } else {
            printf("Error: No response from Storage Server\n");
        }
        free(response);
        conn_close(&conn);
        return;
    }
    free(response);

    char *line = NULL;
    size_t line_cap = 0;
    printf("Client: ");
    fflush(stdout);
    while (getline(&line, &line_cap, stdin) >= 0) {
        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, "ETIRW") == 0) {
            conn_send(&conn, "{\"cmd\":\"ETIRW\"}");
            response = conn_recv(&conn);
            if (!response) {
                printf("Write Failed: No response from server\n");
            } else if (response_is_ok(response)) {
                printf("Write Successful!\n");
            } else {
                print_error(response, "Write Failed");
            }
            free(response);
            break;
        }

        int word_index = 0;
        int replace = 0;
        const char *text = NULL;
        if (!parse_update_line(line, &word_index, &replace, &text)) {
            printf("Invalid input. Use '<index>[!] <words>'.\n");
            printf("Client: ");
            fflush(stdout);
            continue;
        }

        StrBuf sb;
        sb_init(&sb);
        sb_appendf(&sb, "{\"cmd\":\"UPDATE\",\"word_index\":%d,\"content\":", word_index);
        sb_append_json(&sb, text);
        sb_append(&sb, replace ? ",\"mode\":\"replace\"}" : "}");
        char *update = sb_take(&sb);
        response = update && conn_send(&conn, update) ? conn_recv(&conn) : NULL;
        free(update);
        if (!response) {
            printf("Update Failed: No response from server\n");
            break;
        }
        if (response_is_ok(response)) {
            printf("Update acknowledged.\n");
        } else {
            print_error(response, "Update Failed");
        }
        free(response);

        printf("Client: ");
        fflush(stdout);
    }

    free(line);
    conn_close(&conn);
}

/* The storage server sends one JSON message per word and then
 * {"status":"END"}; it already paces the words. */
void handle_stream(const char *filename) {
    Route route;
    Conn conn;
    if (!valid_name_or_warn(filename) || !get_route("STREAM", filename, &route) || !open_route(&route, &conn)) {
        return;
    }
    if (!send_ss_command(&conn, "STREAM", filename, &route, NULL)) {
        printf("Error: Could not send to Storage Server\n");
        conn_close(&conn);
        return;
    }

    int finished = 0;
    int printed = 0;
    char *message;
    while ((message = conn_recv(&conn)) != NULL) {
        char status[16] = {0};
        parse_json_string(message, "status", status, sizeof(status));
        if (strcmp(status, "END") == 0) {
            finished = 1;
            free(message);
            break;
        }
        if (strcmp(status, "OK") != 0) {
            if (printed) {
                printf("\n");
            }
            print_error(message, "Error");
            finished = 1;
            printed = 0;
            free(message);
            break;
        }
        char *word = json_get_str_alloc(message, "word");
        if (word) {
            printf("%s ", word);
            fflush(stdout);
            printed = 1;
        }
        free(word);
        free(message);
    }

    if (printed) {
        printf("\n");
    }
    if (!finished) {
        printf("Error: storage server disconnected mid-stream.\n");
    }
    conn_close(&conn);
}

void handle_undo(const char *filename) {
    Route route;
    Conn conn;
    if (!valid_name_or_warn(filename) || !get_route("UNDO", filename, &route) || !open_route(&route, &conn)) {
        return;
    }

    char *response = send_ss_command(&conn, "UNDO", filename, &route, NULL) ? conn_recv(&conn) : NULL;
    if (!response) {
        printf("Error: No response from Storage Server\n");
    } else if (response_is_ok(response)) {
        printf("Undo Successful!\n");
    } else {
        print_error(response, "Error");
    }
    free(response);
    conn_close(&conn);
}

void handle_delete(const char *filename) {
    if (!valid_name_or_warn(filename)) {
        return;
    }
    char *fields = filename_fields(filename, NULL);
    char *response = fields ? nm_call("DELETE", fields) : NULL;
    free(fields);
    if (!response) {
        return;
    }
    if (response_is_ok(response)) {
        printf("File '%s' deleted successfully!\n", filename);
    } else {
        print_error(response, "Error");
    }
    free(response);
}

void handle_exec(const char *filename) {
    if (!valid_name_or_warn(filename)) {
        return;
    }
    char *fields = filename_fields(filename, NULL);
    char *response = fields ? nm_call("EXEC", fields) : NULL;
    free(fields);
    if (!response) {
        return;
    }
    if (!response_is_ok(response)) {
        print_error(response, "Error");
        free(response);
        return;
    }

    char *output = json_get_str_alloc(response, "output");
    if (output && output[0]) {
        printf("%s", output);
        if (output[strlen(output) - 1] != '\n') {
            printf("\n");
        }
    }
    free(output);
    if (json_get_bool(response, "timed_out")) {
        printf("[stopped: the command ran past the time limit]\n");
    } else if (json_get_bool(response, "truncated")) {
        printf("[stopped: output passed the 64 KB limit]\n");
    } else {
        int exit_code = parse_json_int(response, "exit_code");
        if (exit_code != 0) {
            printf("[exit code %d]\n", exit_code);
        }
    }
    free(response);
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
    printf("  EXEC <filename>          - Run the file as shell commands on the name server\n\n");
    printf("Access Control:\n");
    printf("  ADDACCESS -R <file> <user>  - Grant read access\n");
    printf("  ADDACCESS -W <file> <user>  - Grant write access\n");
    printf("  REMACCESS <file> <user>     - Remove access\n\n");
    printf("Other:\n");
    printf("  LIST                     - List all users\n");
    printf("  help                     - Show this help\n");
    printf("  exit/quit                - Exit client\n\n");
    printf("Filenames: letters, digits, '.', '_' and '-'; not starting with '.'\n\n");
}
//AI code ends
