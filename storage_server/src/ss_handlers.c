#include "ss_handlers.h"
#include "ss_file_ops.h"
#include "ss_locking.h"
#include "ss_session.h"
#include "ss_utils.h"

extern __thread ClientLogContext g_log_ctx;

void send_json(int client, const char* json) {
    char msg[MAX_MSG];
    int n = snprintf(msg, sizeof(msg), "%s\n", json);
    if (n > 0) {
        ssize_t w = write(client, msg, n);
        (void)w;
    }
    log_event("RESPONSE", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, g_log_ctx.cmd, json);
}

void send_error(int client, const char *reason) {
    char buf[MAX_MSG];
    snprintf(buf, sizeof(buf), "{ \"status\":\"ERR\", \"reason\":\"%s\" }", reason);
    send_json(client, buf);
}

void send_ok_message(int client, const char *msg) {
    if (msg && *msg) {
        char buf[MAX_MSG];
        snprintf(buf, sizeof(buf), "{ \"status\":\"OK\", \"msg\":\"%s\" }", msg);
        send_json(client, buf);
    } else {
        send_json(client, "{ \"status\":\"OK\" }");
    }
}

void send_plain_line(int client, const char *line, size_t len) {
    if (!line) return;
    if (len == (size_t)-1) {
        len = strlen(line);
    }
    if (len > 0) {
        write(client, line, len);
        char preview[128];
        size_t copy_len = len < (sizeof(preview) - 1) ? len : (sizeof(preview) - 1);
        memcpy(preview, line, copy_len);
        preview[copy_len] = '\0';
        log_event("RESPONSE", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, g_log_ctx.cmd, preview);
    } else {
        log_event("RESPONSE", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, g_log_ctx.cmd, "");
    }
    write(client, "", 1);
}

void handle_create_file(int client, const char *filename, const char *initial_content) {
    if (!filename || !*filename) {
        send_error(client, "BAD_REQUEST");
        return;
    }

    char path[1024];
    build_filepath(path, filename);

    if (file_exists(path)) {
        send_error(client, "ALREADY_EXISTS");
        return;
    }

    const char *content = initial_content ? initial_content : "";
    if (save_file_atomic(filename, content) != 0) {
        send_error(client, "UNKNOWN");
        return;
    }

    send_ok_message(client, "CREATED");
}

void handle_read(int client, const char *filename) {
    char *content = load_file(filename);
    if (!content) {
        send_error(client, "FILE_NOT_FOUND");
        return;
    }

    char *escaped = json_escape(content);
    if (!escaped) {
        free(content);
        send_error(client, "UNKNOWN");
        return;
    }

    char response[MAX_MSG];
    snprintf(response, sizeof(response),
             "{ \"status\":\"OK\", \"content\":\"%s\" }",
             escaped);

    send_json(client, response);
    free(escaped);
    free(content);
}

void handle_stream(int client, const char *filename) {
    char *content = load_file(filename);
    if (!content) {
        send_error(client, "FILE_NOT_FOUND");
        return;
    }

    const char *p = content;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len > 0) {
            send_plain_line(client, start, len);
            usleep(100000);
        }
    }

    send_plain_line(client, "STOP", 4);
    free(content);
}

void handle_stat(int client, const char *filename) {
    char *content = load_file(filename);
    if (!content) {
        send_error(client, "FILE_NOT_FOUND");
        return;
    }

    int word_count = 0;
    int char_count = 0;
    const char *p = content;
    int in_word = 0;

    while (*p) {
        if (isspace((unsigned char)*p)) {
            if (in_word) {
                word_count++;
                in_word = 0;
            }
        } else {
            char_count++;
            in_word = 1;
        }
        p++;
    }
    if (in_word) {
        word_count++;
    }

    size_t byte_count = strlen(content);
    free(content);

    char response[256];
    snprintf(response, sizeof(response),
             "{\"status\":\"OK\",\"words\":%d,\"chars\":%d,\"bytes\":%zu}",
             word_count, char_count, byte_count);
    send_json(client, response);
}

void handle_undo(int client, const char *filename) {
    if (file_has_active_lock(filename)) {
        send_error(client, "LOCKED");
        return;
    }

    char path[1024];
    build_snapshot_path(path, filename);
    FILE *f = fopen(path, "r");
    if (!f) {
        send_error(client, "NO_SNAPSHOT");
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        send_error(client, "UNKNOWN");
        return;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        send_error(client, "UNKNOWN");
        return;
    }
    rewind(f);

    char *content = malloc((size_t)size + 1);
    if (!content) {
        fclose(f);
        send_error(client, "UNKNOWN");
        return;
    }
    size_t read = fread(content, 1, (size_t)size, f);
    fclose(f);
    content[read] = '\0';

    if (save_file_atomic(filename, content) != 0) {
        free(content);
        send_error(client, "UNKNOWN");
        return;
    }

    free(content);
    send_ok_message(client, NULL);
}
