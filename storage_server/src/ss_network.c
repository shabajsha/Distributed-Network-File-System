#include "ss_network.h"
#include "ss_utils.h"
#include "ss_file_ops.h"
#include "ss_handlers.h"
#include "ss_session.h"
#include "ss_locking.h"

extern __thread ClientLogContext g_log_ctx;

static char g_advertised_ip[INET_ADDRSTRLEN];
static pthread_mutex_t g_advertise_mutex = PTHREAD_MUTEX_INITIALIZER;

// Signs a message to the name server with the shared secret, proving this
// is a genuine storage server.
static void sign_for_nm(const char *kind, const char *filename, long *expires, char out[DNFS_HEX_LEN + 1]) {
    char user[32];
    snprintf(user, sizeof(user), "ss:%d", CLIENT_PORT);
    *expires = (long)time(NULL) + 60;
    dnfs_make_ticket(SS_SECRET, user, filename ? filename : "", kind, *expires, out);
}

// Works out which IP to advertise, from the address used to reach the NM.
static void choose_advertised_ip(int nm_sock) {
    char local_ip[INET_ADDRSTRLEN] = "127.0.0.1";

    if (ADVERTISE_IP[0] != '\0') {
        snprintf(local_ip, sizeof(local_ip), "%s", ADVERTISE_IP);
    } else {
        struct sockaddr_in local_addr;
        socklen_t local_len = sizeof(local_addr);
        if (getsockname(nm_sock, (struct sockaddr *)&local_addr, &local_len) == 0) {
            inet_ntop(AF_INET, &local_addr.sin_addr, local_ip, sizeof(local_ip));
        }
        if (is_loopback_address(local_ip)) {
            char detected[INET_ADDRSTRLEN];
            if (choose_non_loopback_ipv4(detected, sizeof(detected))) {
                snprintf(local_ip, sizeof(local_ip), "%s", detected);
            }
        }
    }

    pthread_mutex_lock(&g_advertise_mutex);
    snprintf(g_advertised_ip, sizeof(g_advertised_ip), "%s", local_ip);
    pthread_mutex_unlock(&g_advertise_mutex);
}

static void advertised_ip(char *out, size_t size) {
    pthread_mutex_lock(&g_advertise_mutex);
    snprintf(out, size, "%s", g_advertised_ip[0] ? g_advertised_ip : "127.0.0.1");
    pthread_mutex_unlock(&g_advertise_mutex);
}

// Sends one message to the name server and returns its reply (caller frees).
static char *nm_exchange(const char *msg, int timeout) {
    int sock = dnfs_connect(NM_IP, dnfs_nm_port(), timeout);
    if (sock < 0) {
        return NULL;
    }
    char *reply = NULL;
    if (dnfs_send_line(sock, msg) == 0) {
        reply = dnfs_recv_line(sock);
    }
    close(sock);
    return reply;
}

static int reply_ok(const char *reply) {
    char status[16];
    return reply && json_get_string(reply, "status", status, sizeof(status)) && strcmp(status, "OK") == 0;
}

// Registers with the name server, sending the list of files on disk.
static int register_with_nm(void) {
    // Pick the advertised IP from a throwaway connection first.
    int probe = dnfs_connect(NM_IP, dnfs_nm_port(), 5);
    if (probe < 0) {
        return 0;
    }
    choose_advertised_ip(probe);
    close(probe);

    char ip[INET_ADDRSTRLEN];
    advertised_ip(ip, sizeof(ip));
    long expires;
    char auth[DNFS_HEX_LEN + 1];
    sign_for_nm("register_ss", "", &expires, auth);

    char *files_json = build_files_manifest();
    StrBuf sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"cmd\":\"register_ss\",\"ip\":\"%s\",\"client_port\":%d,\"expires\":%ld,\"auth\":\"%s\",\"files\":%s}",
               ip, CLIENT_PORT, expires, auth, files_json ? files_json : "[]");
    free(files_json);
    char *msg = sb_take(&sb);
    if (!msg) {
        return 0;
    }

    char *reply = nm_exchange(msg, 30);
    free(msg);
    int ok = reply_ok(reply);
    if (ok) {
        printf("[SS] Registered with Name Server %s:%d as %s:%d\n", NM_IP, dnfs_nm_port(), ip, CLIENT_PORT);
        log_event("INFO", NM_IP, dnfs_nm_port(), "-", "REGISTER_SS", "Registered");
    } else {
        printf("[SS] Registration rejected: %s\n", reply ? reply : "no reply");
        log_event("ERROR", NM_IP, dnfs_nm_port(), "-", "REGISTER_SS", reply ? reply : "no reply");
    }
    free(reply);
    return ok;
}

// Returns 1 if the NM still knows this server, 0 if it must register again,
// -1 if the NM could not be reached.
static int send_heartbeat(void) {
    char ip[INET_ADDRSTRLEN];
    advertised_ip(ip, sizeof(ip));
    long expires;
    char auth[DNFS_HEX_LEN + 1];
    sign_for_nm("ss_heartbeat", "", &expires, auth);

    char msg[512];
    snprintf(msg, sizeof(msg),
             "{\"cmd\":\"ss_heartbeat\",\"ip\":\"%s\",\"client_port\":%d,\"expires\":%ld,\"auth\":\"%s\"}",
             ip, CLIENT_PORT, expires, auth);
    char *reply = nm_exchange(msg, 5);
    if (!reply) {
        return -1;
    }
    int ok = reply_ok(reply);
    free(reply);
    return ok ? 1 : 0;
}

// Keeps this server registered: retries until the NM answers, sends a
// heartbeat every few seconds, and registers again if the NM restarted.
void *heartbeat_thread(void *arg) {
    (void)arg;
    int interval = dnfs_env_int("DNFS_HEARTBEAT_INTERVAL", 5);
    int registered = 0;
    int warned = 0;

    for (;;) {
        if (!registered) {
            registered = register_with_nm();
            if (!registered && !warned) {
                printf("[SS] Name Server %s:%d not reachable yet; retrying every %d s\n",
                       NM_IP, dnfs_nm_port(), interval);
                warned = 1;
            }
            if (registered) {
                warned = 0;
            }
        } else if (send_heartbeat() == 0) {
            registered = 0;
            continue;
        }
        sleep((unsigned int)interval);
    }
    return NULL;
}

// Tells the name server a file changed, so it can copy it to the backup.
void notify_nm_commit(const char *filename) {
    char ip[INET_ADDRSTRLEN];
    advertised_ip(ip, sizeof(ip));
    long expires;
    char auth[DNFS_HEX_LEN + 1];
    sign_for_nm("ss_commit", filename, &expires, auth);

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "{\"cmd\":\"ss_commit\",\"ip\":\"%s\",\"client_port\":%d,\"filename\":\"%s\",\"expires\":%ld,\"auth\":\"%s\"}",
             ip, CLIENT_PORT, filename, expires, auth);
    char *reply = nm_exchange(msg, 30);
    if (!reply_ok(reply)) {
        log_event("ERROR", NM_IP, dnfs_nm_port(), "-", "SS_COMMIT", reply ? reply : "Name Server unreachable");
    }
    free(reply);
}

static bool needs_ticket(const char *cmd) {
    return strcmp(cmd, "READ") == 0 || strcmp(cmd, "STREAM") == 0 || strcmp(cmd, "WRITE") == 0 ||
           strcmp(cmd, "UNDO") == 0 || strcmp(cmd, "CREATE") == 0 || strcmp(cmd, "DELETE") == 0 ||
           strcmp(cmd, "STAT") == 0 || strcmp(cmd, "PUT") == 0;
}

static bool name_server_only(const char *cmd) {
    return strcmp(cmd, "CREATE") == 0 || strcmp(cmd, "DELETE") == 0 ||
           strcmp(cmd, "STAT") == 0 || strcmp(cmd, "PUT") == 0;
}

static void parse_and_handle(int client, char *buf, WriteSession *session) {
    char cmd[32];
    if (!json_get_string(buf, "cmd", cmd, sizeof(cmd))) {
        g_log_ctx.cmd[0] = '\0';
        log_event("REQUEST", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, "UNKNOWN", buf);
        send_error(client, "UNKNOWN_CMD");
        return;
    }
    snprintf(g_log_ctx.cmd, sizeof(g_log_ctx.cmd), "%s", cmd);

    // Every command that names a file must carry a ticket signed by the name
    // server for this user, file and command. UPDATE and ETIRW belong to the
    // WRITE session that was already checked.
    char filename[MAX_FILENAME] = {0};
    if (needs_ticket(cmd)) {
        char user[MAX_USERNAME] = {0};
        char ticket[DNFS_HEX_LEN + 1] = {0};
        long expires = 0;
        if (!json_get_string(buf, "filename", filename, sizeof(filename)) || !dnfs_valid_filename(filename)) {
            send_error(client, "INVALID_FILENAME");
            return;
        }
        json_get_string(buf, "user", user, sizeof(user));
        json_get_string(buf, "ticket", ticket, sizeof(ticket));
        json_get_long(buf, "expires", &expires);
        if (!dnfs_check_ticket(SS_SECRET, user, filename, cmd, expires, ticket) ||
            (name_server_only(cmd) && strcmp(user, "nm") != 0)) {
            log_event("REQUEST", g_log_ctx.ip, g_log_ctx.port, user, cmd, "Rejected: invalid ticket");
            send_error(client, "UNAUTHORIZED");
            return;
        }
        snprintf(g_log_ctx.username, sizeof(g_log_ctx.username), "%s", user);
    } else if (session && session->username[0] != '\0') {
        snprintf(g_log_ctx.username, sizeof(g_log_ctx.username), "%s", session->username);
    }

    char preview[201];
    snprintf(preview, sizeof(preview), "%.200s", buf);
    log_event("REQUEST", g_log_ctx.ip, g_log_ctx.port, g_log_ctx.username, g_log_ctx.cmd, preview);

    if (strcmp(cmd, "READ") == 0) {
        bool is_nm = strcmp(g_log_ctx.username, "nm") == 0;
        handle_read(client, filename, is_nm && json_get_bool(buf, "with_snapshot"));
        return;
    }

    if (strcmp(cmd, "CREATE") == 0) {
        char *content = json_get_str_alloc(buf, "content");
        handle_create_file(client, filename, content ? content : "");
        free(content);
        return;
    }

    if (strcmp(cmd, "PUT") == 0) {
        char *content = json_get_str_alloc(buf, "content");
        char *snapshot = json_get_str_alloc(buf, "snapshot");
        if (!content) {
            send_error(client, "BAD_REQUEST");
        } else {
            handle_put(client, filename, content, snapshot);
        }
        free(content);
        free(snapshot);
        return;
    }

    if (strcmp(cmd, "WRITE") == 0) {
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
        char *content = json_get_str_alloc(buf, "content");
        if (!content) {
            send_error(client, "BAD_REQUEST");
            return;
        }
        char mode[16] = {0};
        int replace_word = json_get_string(buf, "mode", mode, sizeof(mode)) && strcmp(mode, "replace") == 0;
        handle_update(client, word_index, content, session, replace_word);
        free(content);
        return;
    }

    if (strcmp(cmd, "ETIRW") == 0) {
        char committed[MAX_FILENAME];
        snprintf(committed, sizeof(committed), "%s", session->filename);
        if (handle_commit(client, session)) {
            notify_nm_commit(committed);
        }
        return;
    }

    if (strcmp(cmd, "UNDO") == 0) {
        if (handle_undo(client, filename)) {
            notify_nm_commit(filename);
        }
        return;
    }

    if (strcmp(cmd, "STREAM") == 0) {
        handle_stream(client, filename);
        return;
    }

    if (strcmp(cmd, "STAT") == 0) {
        handle_stat(client, filename);
        return;
    }

    if (strcmp(cmd, "DELETE") == 0) {
        handle_delete(client, filename);
        return;
    }

    send_error(client, "UNKNOWN_CMD");
}

void handle_client(int client_sock) {
    WriteSession session;
    session_init(&session, client_sock);

    LineReader reader;
    line_reader_init(&reader, client_sock);

    char *line;
    while ((line = line_reader_next(&reader)) != NULL) {
        if (line[0] != '\0') {
            printf("[SS] Received: %.200s\n", line);
            parse_and_handle(client_sock, line, &session);
        }
        free(line);
    }
    if (reader.too_long) {
        send_error(client_sock, "MESSAGE_TOO_LARGE");
    }
    line_reader_free(&reader);

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
        snprintf(g_log_ctx.ip, sizeof(g_log_ctx.ip), "unknown");
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
