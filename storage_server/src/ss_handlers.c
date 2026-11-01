#include "ss_handlers.h"
#include "ss_file_ops.h"
#include "ss_locking.h"
#include "ss_session.h"
#include "ss_utils.h"

extern __thread ClientLogContext g_log_ctx;

// Sends one newline-terminated JSON message. Only the first 200 characters
// go to the log, so reading a large file doesn't copy it into the log.
// Returns -1 if the client is gone.
static int send_json_checked(int client, const char *json) {
    int rc = dnfs_send_line(client, json);
    char preview[201];
    snprintf(preview, sizeof(preview), "%.200s", json);
    log_event("RESPONSE", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, g_log_ctx.cmd, preview);
    return rc;
}

void send_json(int client, const char *json) {
    send_json_checked(client, json);
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

void handle_create_file(int client, const char *filename, const char *initial_content) {
    char path[1024];
    build_filepath(path, sizeof(path), filename);

    file_lock(filename);
    if (file_exists(path)) {
        file_unlock(filename);
        send_error(client, "ALREADY_EXISTS");
        return;
    }
    int rc = save_file_atomic(filename, initial_content ? initial_content : "");
    file_unlock(filename);

    if (rc != 0) {
        send_error(client, "UNKNOWN");
        return;
    }
    send_ok_message(client, "CREATED");
}

// The reply is sized to the file, so there is no length limit.
// with_snapshot is used by the name server when copying a file to its backup.
void handle_read(int client, const char *filename, int with_snapshot) {
    char *content = load_file(filename);
    if (!content) {
        send_error(client, "FILE_NOT_FOUND");
        return;
    }
    char *snapshot = with_snapshot ? load_snapshot(filename) : NULL;

    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{ \"status\":\"OK\", \"content\":");
    sb_append_json(&sb, content);
    if (snapshot) {
        sb_append(&sb, ", \"snapshot\":");
        sb_append_json(&sb, snapshot);
    }
    sb_append(&sb, " }");
    free(content);
    free(snapshot);

    char *response = sb_take(&sb);
    if (!response) {
        send_error(client, "UNKNOWN");
        return;
    }
    send_json(client, response);
    free(response);
}

// Each word is its own JSON message and the end is {"status":"END"}, so a
// word like "STOP" or "{" in the file can't be mistaken for a control
// message. Stops as soon as the client goes away.
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

        char *word = strndup(start, (size_t)(p - start));
        StrBuf sb;
        sb_init(&sb);
        sb_append(&sb, "{ \"status\":\"OK\", \"word\":");
        sb_append_json(&sb, word);
        sb_append(&sb, " }");
        free(word);
        char *msg = sb_take(&sb);
        int rc = msg ? send_json_checked(client, msg) : -1;
        free(msg);
        if (rc != 0) {
            free(content);
            return;
        }
        usleep(100000);
    }

    send_json(client, "{ \"status\":\"END\" }");
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

// Single-level undo: restores the version saved just before the last
// change, then discards that snapshot.
int handle_undo(int client, const char *filename) {
    file_lock(filename);
    if (file_has_active_lock(filename)) {
        file_unlock(filename);
        send_error(client, "LOCKED");
        return 0;
    }

    char *snapshot = load_snapshot(filename);
    if (!snapshot) {
        file_unlock(filename);
        send_error(client, "NO_SNAPSHOT");
        return 0;
    }

    int rc = save_file_atomic(filename, snapshot);
    if (rc == 0) {
        remove_snapshot(filename);
    }
    file_unlock(filename);
    free(snapshot);

    if (rc != 0) {
        send_error(client, "UNKNOWN");
        return 0;
    }
    send_ok_message(client, NULL);
    return 1;
}

// Replaces a file with a copy from the other replica (name server only).
void handle_put(int client, const char *filename, const char *content, const char *snapshot) {
    file_lock(filename);
    int rc = save_file_atomic(filename, content);
    if (rc == 0) {
        if (snapshot) {
            rc = save_snapshot(filename, snapshot);
        } else {
            remove_snapshot(filename);
        }
    }
    file_unlock(filename);

    if (rc != 0) {
        send_error(client, "UNKNOWN");
        return;
    }
    send_ok_message(client, "STORED");
}

void handle_delete(int client, const char *filename) {
    file_lock(filename);
    if (file_has_active_lock(filename)) {
        file_unlock(filename);
        send_error(client, "LOCKED");
        return;
    }

    char path[1024];
    build_filepath(path, sizeof(path), filename);
    int rc = remove(path);
    if (rc == 0) {
        remove_snapshot(filename);
    }
    file_unlock(filename);

    if (rc != 0) {
        send_error(client, "FILE_NOT_FOUND");
        return;
    }
    send_ok_message(client, NULL);
}
