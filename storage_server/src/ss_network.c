#include "ss_network.h"
#include "ss_utils.h"
#include "ss_file_ops.h"
#include "ss_handlers.h"
#include "ss_session.h"
#include "ss_locking.h"

extern __thread ClientLogContext g_log_ctx;

void register_with_nm(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[SS] socket");
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NM_PORT);
    if (inet_pton(AF_INET, NM_IP, &addr.sin_addr) <= 0) {
        perror("[SS] inet_pton");
        close(sock);
        return;
    }

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[SS] connect NM");
        close(sock);
        return;
    }
    
    printf("[SS] Connected to Name Server successfully\n");

    char local_ip[INET_ADDRSTRLEN];
    
    if (ADVERTISE_IP[0] != '\0') {
        strncpy(local_ip, ADVERTISE_IP, sizeof(local_ip));
        local_ip[sizeof(local_ip) - 1] = '\0';
    } else {
        struct sockaddr_in local_addr;
        socklen_t local_len = sizeof(local_addr);
        if (getsockname(sock, (struct sockaddr *)&local_addr, &local_len) == 0) {
            if (inet_ntop(AF_INET, &local_addr.sin_addr, local_ip, sizeof(local_ip)) == NULL) {
                strncpy(local_ip, "127.0.0.1", sizeof(local_ip));
            }
        } else {
            strncpy(local_ip, "127.0.0.1", sizeof(local_ip));
        }

        if (is_loopback_address(local_ip)) {
            char detected[INET_ADDRSTRLEN];
            if (choose_non_loopback_ipv4(detected, sizeof(detected))) {
                strncpy(local_ip, detected, sizeof(local_ip) - 1);
                local_ip[sizeof(local_ip) - 1] = '\0';
                printf("[SS] Detected non-loopback IP %s for advertisement\n", local_ip);
            } else {
                printf("[SS] WARNING: Unable to find non-loopback interface, advertising %s\n", local_ip);
            }
        }
    }
    
    printf("[SS] Registering with NM using IP: %s (port: %d)\n", local_ip, CLIENT_PORT);

    char *files_json = build_files_manifest();
    if (!files_json) {
        files_json = strdup("[]");
    }

    size_t payload_len = strlen(files_json) + 200;
    char *msg = (char *)malloc(payload_len);
    if (!msg) {
        free(files_json);
        close(sock);
        return;
    }

    int n = snprintf(msg, payload_len,
                     "{ \"cmd\":\"register_ss\", \"ip\":\"%s\", \"nm_port\":%d, \"client_port\":%d, \"files\":%s }\n",
                     local_ip, NM_PORT, CLIENT_PORT, files_json);
    free(files_json);

    if (n > 0) {
        char preview[256];
        size_t copy_len = (size_t)n < sizeof(preview) - 1 ? (size_t)n : sizeof(preview) - 1;
        memcpy(preview, msg, copy_len);
        preview[copy_len] = '\0';
        char *newline = strchr(preview, '\n');
        if (newline) *newline = '\0';
        log_event("REQUEST", "127.0.0.1", NM_PORT, "-", "REGISTER_SS", preview);
        if (write(sock, msg, (size_t)n) < 0) {
            perror("[SS] register send");
            free(msg);
            close(sock);
            return;
        }
    }
    free(msg);

    printf("[SS] Waiting for NM response...\n");
    char buf[256];
    ssize_t r = read(sock, buf, sizeof(buf) - 1);
    if (r > 0) {
        buf[r] = '\0';
        printf("[SS] NM reply: %s\n", buf);
        log_event("RESPONSE", "127.0.0.1", NM_PORT, "-", "REGISTER_SS", buf);
    } else if (r == 0) {
        printf("[SS] WARNING: Name Server closed connection without response\n");
        log_event("ERROR", "127.0.0.1", NM_PORT, "-", "REGISTER_SS", "Connection closed");
    } else {
        perror("[SS] read NM response");
        log_event("ERROR", "127.0.0.1", NM_PORT, "-", "REGISTER_SS", "Read timeout or error");
    }

    close(sock);
}

static void parse_and_handle(int client, char *buf, WriteSession *session) {
    char cmd[32];
    if (!json_get_string(buf, "cmd", cmd, sizeof(cmd))) {
        g_log_ctx.cmd[0] = '\0';
        g_log_ctx.username[0] = '\0';
        log_event("REQUEST", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, "UNKNOWN", buf);
        send_error(client, "UNKNOWN_CMD");
        return;
    }

    char username[MAX_USERNAME] = {0};
    json_get_string(buf, "username", username, sizeof(username));
    if (username[0] != '\0') {
        strncpy(g_log_ctx.username, username, sizeof(g_log_ctx.username) - 1);
        g_log_ctx.username[sizeof(g_log_ctx.username) - 1] = '\0';
    } else if (session && session->username[0] != '\0') {
        strncpy(g_log_ctx.username, session->username, sizeof(g_log_ctx.username) - 1);
        g_log_ctx.username[sizeof(g_log_ctx.username) - 1] = '\0';
    } else {
        g_log_ctx.username[0] = '\0';
    }

    strncpy(g_log_ctx.cmd, cmd, sizeof(g_log_ctx.cmd) - 1);
    g_log_ctx.cmd[sizeof(g_log_ctx.cmd) - 1] = '\0';
    log_event("REQUEST", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, g_log_ctx.cmd, buf);

    if (strcmp(cmd, "READ") == 0) {
        char filename[MAX_FILENAME];
        if (!json_get_string(buf, "filename", filename, sizeof(filename))) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        handle_read(client, filename);
        return;
    }

    if (strcmp(cmd, "CREATE") == 0) {
        char filename[MAX_FILENAME];
        if (!json_get_string(buf, "filename", filename, sizeof(filename))) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        char content[MAX_MSG];
        if (!json_get_string(buf, "content", content, sizeof(content))) {
            content[0] = '\0';
        }
        handle_create_file(client, filename, content);
        return;
    }

    if (strcmp(cmd, "WRITE") == 0) {
        char filename[MAX_FILENAME];
        if (!json_get_string(buf, "filename", filename, sizeof(filename))) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        int sentence_index = 0;
        if (!json_get_int(buf, "sentence_index", &sentence_index)) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        handle_write_begin(client, filename, sentence_index, session, g_log_ctx.username);
        return;
    }

    if (strcmp(cmd, "UPDATE") == 0) {
        int word_index = 0;
        if (!json_get_int(buf, "word_index", &word_index)) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        char content[MAX_MSG];
        if (!json_get_string(buf, "content", content, sizeof(content))) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        char mode[16] = {0};
        int replace_word = 0;
        if (json_get_string(buf, "mode", mode, sizeof(mode))) {
            if (strcmp(mode, "replace") == 0) {
                replace_word = 1;
            }
        }
        handle_update(client, word_index, content, session, replace_word);
        return;
    }

    if (strcmp(cmd, "ETIRW") == 0) {
        handle_commit(client, session);
        return;
    }

    if (strcmp(cmd, "UNDO") == 0) {
        char filename[MAX_FILENAME];
        if (!json_get_string(buf, "filename", filename, sizeof(filename))) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        handle_undo(client, filename);
        return;
    }

    if (strcmp(cmd, "STREAM") == 0) {
        char filename[MAX_FILENAME];
        if (!json_get_string(buf, "filename", filename, sizeof(filename))) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        handle_stream(client, filename);
        return;
    }

    if (strcmp(cmd, "STAT") == 0) {
        char filename[MAX_FILENAME];
        if (!json_get_string(buf, "filename", filename, sizeof(filename))) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        handle_stat(client, filename);
        return;
    }

    if (strcmp(cmd, "DELETE") == 0) {
        char filename[MAX_FILENAME];
        if (!json_get_string(buf, "filename", filename, sizeof(filename))) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", DATA_DIR, filename);
        if (remove(filepath) == 0) {
            char snappath[1024];
            snprintf(snappath, sizeof(snappath), "%s/%s.bak", SNAP_DIR, filename);
            remove(snappath);
            send_ok_message(client, NULL);
        } else {
            send_error(client, "FILE_NOT_FOUND");
        }
        return;
    }

    send_error(client, "UNKNOWN_CMD");
}

void handle_client(int client_sock) {
    char buffer[MAX_MSG];
    ssize_t bytes_read;

    char workbuf[MAX_MSG];
    size_t worklen = 0;
    workbuf[0] = '\0';

    WriteSession session;
    session_init(&session, client_sock);

    while (1) {
        memset(buffer, 0, sizeof(buffer));
        bytes_read = read(client_sock, buffer, sizeof(buffer) - 1);

        if (bytes_read <= 0) {
            break;
        }

        if (worklen + (size_t)bytes_read >= sizeof(workbuf) - 1) {
            worklen = 0;
        }

        memcpy(workbuf + worklen, buffer, bytes_read);
        worklen += bytes_read;
        workbuf[worklen] = '\0';

        char *line_start = workbuf;
        while (1) {
            char *nl = strchr(line_start, '\n');
            if (!nl) break;

            *nl = '\0';

            size_t len = strlen(line_start);
            if (len > 0 && line_start[len - 1] == '\r') {
                line_start[len - 1] = '\0';
            }

            if (strlen(line_start) > 0) {
                printf("[SS] Received: %s\n", line_start);
                log_event("INFO", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, 
                         g_log_ctx.cmd, line_start);
                parse_and_handle(client_sock, line_start, &session);
            }

            line_start = nl + 1;
        }

        size_t remaining = strlen(line_start);
        memmove(workbuf, line_start, remaining + 1);
        worklen = remaining;
    }

    session_reset(&session);
    release_sentence_locks_for_owner(client_sock);
}

void *client_thread(void *arg) {
    ClientThreadArg *ctx = (ClientThreadArg *)arg;
    if (!ctx) return NULL;

    int client_sock = ctx->socket_fd;
    struct sockaddr_in addr = ctx->addr;
    free(ctx);

    if (!inet_ntop(AF_INET, &addr.sin_addr, g_log_ctx.ip, sizeof(g_log_ctx.ip))) {
        strncpy(g_log_ctx.ip, "unknown", sizeof(g_log_ctx.ip) - 1);
        g_log_ctx.ip[sizeof(g_log_ctx.ip) - 1] = '\0';
    }
    g_log_ctx.port = ntohs(addr.sin_port);
    g_log_ctx.username[0] = '\0';
    g_log_ctx.cmd[0] = '\0';

    log_event("INFO", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, 
             "CONNECT", "Client connected");

    handle_client(client_sock);
    close(client_sock);

    log_event("INFO", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, 
             "DISCONNECT", "Client disconnected");
    return NULL;
}
