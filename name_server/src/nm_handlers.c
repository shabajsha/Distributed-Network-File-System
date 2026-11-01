#include "nm_handlers.h"
#include "nm_cache.h"
#include "nm_logging.h"
#include "nm_metadata.h"
#include "nm_network.h"

#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>

#define SS_CALL_TIMEOUT 5
#define STAT_TIMEOUT 2
#define EXEC_OUTPUT_CAP (64 * 1024)

typedef struct {
    char ip[INET_ADDRSTRLEN];
    int port;
    int is_backup;
} Route;

/* ---------- storage server registry ---------- */

/* A storage server is alive if it sent a heartbeat within ss_timeout_sec. */
static int ss_alive(const char *ip, int port) {
    if (!ip || !ip[0] || port <= 0) {
        return 0;
    }
    time_t now = time(NULL);
    int alive = 0;
    pthread_mutex_lock(&ss_mutex);
    for (int i = 0; i < ss_count; i++) {
        if (storage_servers[i].client_port == port && strcmp(storage_servers[i].ip, ip) == 0) {
            alive = now - storage_servers[i].last_seen <= ss_timeout_sec;
            break;
        }
    }
    pthread_mutex_unlock(&ss_mutex);
    return alive;
}

/* Prefer the IP the storage server advertises, unless it is missing or a
 * loopback address while the connection came from elsewhere. */
static void resolve_ss_ip(const char *advertised, const char *observed, char *out, size_t size) {
    struct in_addr tmp;
    int advertised_valid = advertised[0] && inet_pton(AF_INET, advertised, &tmp) == 1;
    int use_observed = !advertised_valid || strcmp(advertised, "0.0.0.0") == 0 ||
                       (strcmp(advertised, "127.0.0.1") == 0 && strcmp(observed, "127.0.0.1") != 0);
    snprintf(out, size, "%s", use_observed ? observed : advertised);
}

/* Storage servers prove they hold the cluster secret by signing
 * (kind, "ss:<port>", file, expires). */
static int verify_ss_auth(const char *request, const char *kind, const char *file, int port) {
    char auth[DNFS_HEX_LEN + 1];
    long expires = 0;
    if (!json_get_str(request, "auth", auth, sizeof(auth)) || !json_get_long(request, "expires", &expires)) {
        return 0;
    }
    char user[32];
    snprintf(user, sizeof(user), "ss:%d", port);
    return dnfs_check_ticket(NM_SECRET, user, file ? file : "", kind, expires, auth);
}

/* ---------- talking to storage servers ---------- */

static char *nm_ss_command(const char *ip, int port, const char *op, const char *filename,
                           const char *extra, int timeout) {
    long expires = (long)time(NULL) + DNFS_TICKET_TTL;
    char ticket[DNFS_HEX_LEN + 1];
    dnfs_make_ticket(NM_SECRET, "nm", filename, op, expires, ticket);

    StrBuf sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"cmd\":\"%s\",\"filename\":", op);
    sb_append_json(&sb, filename);
    sb_appendf(&sb, ",\"user\":\"nm\",\"expires\":%ld,\"ticket\":\"%s\"", expires, ticket);
    sb_append(&sb, extra);
    sb_append(&sb, "}");
    char *request = sb_take(&sb);
    if (!request) {
        return NULL;
    }
    char *response = ss_call(ip, port, request, timeout);
    free(request);
    return response;
}

static int resp_ok(const char *resp) {
    char status[16];
    return resp && json_get_str(resp, "status", status, sizeof(status)) && strcmp(status, "OK") == 0;
}

static void resp_reason(const char *resp, char *out, size_t size) {
    if (!resp) {
        snprintf(out, size, "SS_UNAVAILABLE");
    } else if (!json_get_str(resp, "reason", out, size) || !out[0]) {
        snprintf(out, size, "UNKNOWN");
    }
}

static int request_file_stats(const char *ip, int port, const char *filename,
                              int *words, int *chars, int *bytes) {
    char *resp = nm_ss_command(ip, port, "STAT", filename, NULL, STAT_TIMEOUT);
    int ok = resp_ok(resp);
    if (ok) {
        *words = parse_json_int(resp, "words");
        *chars = parse_json_int(resp, "chars");
        *bytes = parse_json_int(resp, "bytes");
    }
    free(resp);
    return ok;
}

/* ---------- replication ---------- */

/* Copies a file and its undo snapshot from one replica to another.
 * replication_mutex keeps copies in order, so the newest content always
 * lands last. */
static int sync_replica(const char *filename, const char *src_ip, int src_port,
                        const char *dst_ip, int dst_port) {
    pthread_mutex_lock(&replication_mutex);
    int ok = 0;
    char *resp = nm_ss_command(src_ip, src_port, "READ", filename, ",\"with_snapshot\":true", SS_CALL_TIMEOUT);
    if (resp_ok(resp)) {
        char *content = json_get_str_alloc(resp, "content");
        char *snapshot = json_get_str_alloc(resp, "snapshot");
        if (content) {
            StrBuf extra;
            sb_init(&extra);
            sb_append(&extra, ",\"content\":");
            sb_append_json(&extra, content);
            if (snapshot) {
                sb_append(&extra, ",\"snapshot\":");
                sb_append_json(&extra, snapshot);
            }
            char *fields = sb_take(&extra);
            if (fields) {
                char *put = nm_ss_command(dst_ip, dst_port, "PUT", filename, fields, SS_CALL_TIMEOUT);
                ok = resp_ok(put);
                free(put);
                free(fields);
            }
        }
        free(content);
        free(snapshot);
    }
    free(resp);
    pthread_mutex_unlock(&replication_mutex);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Sync %s %s:%d -> %s:%d %s",
             filename, src_ip, src_port, dst_ip, dst_port, ok ? "done" : "FAILED");
    log_message(ok ? "INFO" : "ERROR", log_msg, dst_ip, dst_port, "replication");
    return ok;
}

static void set_replica_stale(const char *filename, const char *ip, int port, int stale) {
    pthread_mutex_lock(&files_mutex);
    FileMetadata *file = lookup_file(filename);
    if (file) {
        if (file->ss_port == port && strcmp(file->ss_ip, ip) == 0) {
            file->primary_stale = stale;
        } else if (file->backup_ss_port == port && strcmp(file->backup_ss_ip, ip) == 0) {
            file->backup_stale = stale;
        }
    }
    pthread_mutex_unlock(&files_mutex);
}

/* Picks the replica that should serve a request: an up-to-date one that is
 * alive, refreshing a stale primary from the backup first when possible. */
static int choose_replica(const char *filename, Route *route) {
    char p_ip[INET_ADDRSTRLEN], b_ip[INET_ADDRSTRLEN];
    int p_port, b_port, p_stale, b_stale;

    pthread_mutex_lock(&files_mutex);
    FileMetadata *file = lookup_file(filename);
    if (!file) {
        pthread_mutex_unlock(&files_mutex);
        return 0;
    }
    snprintf(p_ip, sizeof(p_ip), "%s", file->ss_ip);
    snprintf(b_ip, sizeof(b_ip), "%s", file->backup_ss_ip);
    p_port = file->ss_port;
    b_port = file->backup_ss_port;
    p_stale = file->primary_stale;
    b_stale = file->backup_stale;
    pthread_mutex_unlock(&files_mutex);

    int p_alive = ss_alive(p_ip, p_port);
    int b_alive = b_port ? ss_alive(b_ip, b_port) : 0;

    if (p_alive && p_stale && b_alive && !b_stale &&
        sync_replica(filename, b_ip, b_port, p_ip, p_port)) {
        set_replica_stale(filename, p_ip, p_port, 0);
        p_stale = 0;
        save_metadata();
    }

    int use_backup;
    if (p_alive && !p_stale) {
        use_backup = 0;
    } else if (b_alive && !b_stale) {
        use_backup = 1;
    } else if (p_alive) {
        use_backup = 0;
    } else if (b_alive) {
        use_backup = 1;
    } else {
        return 0;
    }

    snprintf(route->ip, sizeof(route->ip), "%s", use_backup ? b_ip : p_ip);
    route->port = use_backup ? b_port : p_port;
    route->is_backup = use_backup;
    return 1;
}

/* ---------- registration ---------- */

void handle_register_client(int client_fd, const char *request, const char *client_ip) {
    char username[MAX_USERNAME] = {0};
    char password[256] = {0};
    parse_json_string(request, "username", username, sizeof(username));
    parse_json_string(request, "password", password, sizeof(password));

    if (!dnfs_valid_username(username)) {
        send_error(client_fd, "INVALID_USERNAME");
        return;
    }
    if (!password[0]) {
        send_error(client_fd, "PASSWORD_REQUIRED");
        return;
    }

    char stored[PASS_HASH_LEN] = {0};
    pthread_mutex_lock(&users_mutex);
    int idx = user_index(username);
    if (idx >= 0) {
        snprintf(stored, sizeof(stored), "%s", users[idx].pass_hash);
    }
    pthread_mutex_unlock(&users_mutex);

    /* Hashing is slow on purpose, so it happens outside the lock. The first
     * login for a name (or for a user saved before passwords existed) sets
     * the password. */
    int setting_password = 0;
    if (stored[0]) {
        if (!dnfs_verify_password(password, stored)) {
            log_message("INFO", "Login failed: wrong password", client_ip, 0, username);
            send_error(client_fd, "BAD_CREDENTIALS");
            return;
        }
    } else {
        if (!dnfs_hash_password(password, stored, sizeof(stored))) {
            send_error(client_fd, "UNKNOWN");
            return;
        }
        setting_password = 1;
    }

    char token[DNFS_HEX_LEN + 1];
    if (!dnfs_random_hex(token, DNFS_HEX_LEN / 2)) {
        send_error(client_fd, "UNKNOWN");
        return;
    }

    pthread_mutex_lock(&users_mutex);
    idx = user_index(username);
    if (idx < 0) {
        if (user_count >= MAX_USERS) {
            pthread_mutex_unlock(&users_mutex);
            send_error(client_fd, "MAX_USERS_REACHED");
            return;
        }
        idx = user_count++;
        snprintf(users[idx].username, sizeof(users[idx].username), "%s", username);
        snprintf(users[idx].pass_hash, sizeof(users[idx].pass_hash), "%s", stored);
    } else if (!users[idx].pass_hash[0]) {
        snprintf(users[idx].pass_hash, sizeof(users[idx].pass_hash), "%s", stored);
    } else if (setting_password) {
        /* Someone else set this user's password a moment ago. */
        pthread_mutex_unlock(&users_mutex);
        send_error(client_fd, "BAD_CREDENTIALS");
        return;
    }
    snprintf(users[idx].ip, sizeof(users[idx].ip), "%s", client_ip);
    users[idx].last_login = time(NULL);

    int slot = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].token[0]) {
            slot = i;
            break;
        }
        if (sessions[i].last_used < sessions[slot].last_used) {
            slot = i;
        }
    }
    snprintf(sessions[slot].token, sizeof(sessions[slot].token), "%s", token);
    snprintf(sessions[slot].username, sizeof(sessions[slot].username), "%s", username);
    sessions[slot].last_used = time(NULL);
    pthread_mutex_unlock(&users_mutex);

    if (setting_password) {
        save_metadata();
    }

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Client logged in: %s", username);
    log_message("INFO", log_msg, client_ip, 0, username);

    char response[160];
    snprintf(response, sizeof(response), "{\"status\":\"OK\",\"token\":\"%s\"}", token);
    send_response(client_fd, response);
}

typedef struct {
    char filename[MAX_FILENAME];
    char ip[INET_ADDRSTRLEN];
    int port;
} SyncJob;

static int name_in_list(char **names, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

void handle_register_ss(int ss_fd, const char *request, const char *ss_ip) {
    char advertised[INET_ADDRSTRLEN] = {0};
    parse_json_string(request, "ip", advertised, sizeof(advertised));
    int client_port = parse_json_int(request, "client_port");
    if (client_port <= 0 || client_port > 65535) {
        send_error(ss_fd, "BAD_REQUEST");
        return;
    }
    if (!verify_ss_auth(request, "register_ss", "", client_port)) {
        log_message("ERROR", "Rejected storage server registration: bad signature", ss_ip, client_port, "SS");
        send_error(ss_fd, "UNAUTHORIZED");
        return;
    }

    char resolved[INET_ADDRSTRLEN];
    resolve_ss_ip(advertised, ss_ip, resolved, sizeof(resolved));

    char old_ip[INET_ADDRSTRLEN] = {0};
    int existing = 0;
    pthread_mutex_lock(&ss_mutex);
    int index = -1;
    for (int i = 0; i < ss_count; i++) {
        if (storage_servers[i].client_port == client_port &&
            (strcmp(storage_servers[i].ip, resolved) == 0 || strcmp(storage_servers[i].ip, ss_ip) == 0 ||
             (advertised[0] && strcmp(storage_servers[i].ip, advertised) == 0))) {
            index = i;
            break;
        }
    }
    if (index >= 0) {
        existing = 1;
        snprintf(old_ip, sizeof(old_ip), "%s", storage_servers[index].ip);
    } else if (ss_count < MAX_STORAGE_SERVERS) {
        index = ss_count++;
        storage_servers[index].client_port = client_port;
    } else {
        pthread_mutex_unlock(&ss_mutex);
        send_error(ss_fd, "MAX_SS_REACHED");
        return;
    }
    snprintf(storage_servers[index].ip, sizeof(storage_servers[index].ip), "%s", resolved);
    storage_servers[index].registered_at = time(NULL);
    storage_servers[index].last_seen = time(NULL);
    pthread_mutex_unlock(&ss_mutex);

    /* The files this server reports holding. */
    char **manifest = NULL;
    int manifest_count = 0;
    const char *p = json_find_value(request, "files");
    if (p && *p == '[') {
        p++;
        for (;;) {
            p = json_skip_ws(p);
            if (*p != '"') {
                break;
            }
            const char *end = NULL;
            char *name = json_read_string(p, &end);
            if (!name) {
                break;
            }
            char **tmp = realloc(manifest, sizeof(char *) * (size_t)(manifest_count + 1));
            if (!tmp) {
                free(name);
                break;
            }
            manifest = tmp;
            manifest[manifest_count++] = name;
            p = json_skip_ws(end);
            if (*p == ',') {
                p++;
            }
        }
    }

    SyncJob *jobs = NULL;
    int job_count = 0;
    int remapped = 0;
    int missing = 0;

    pthread_mutex_lock(&files_mutex);
    for (int i = 0; i < HASH_BUCKETS; i++) {
        for (HashNode *node = file_hash_table[i]; node; node = node->next) {
            FileMetadata *file = node->file;
            if (file->pending) {
                continue;
            }
            /* A server that came back on a new IP keeps its files. */
            if (existing && strcmp(old_ip, resolved) != 0) {
                if (file->ss_port == client_port && strcmp(file->ss_ip, old_ip) == 0) {
                    snprintf(file->ss_ip, sizeof(file->ss_ip), "%s", resolved);
                    remapped++;
                }
                if (file->backup_ss_port == client_port && strcmp(file->backup_ss_ip, old_ip) == 0) {
                    snprintf(file->backup_ss_ip, sizeof(file->backup_ss_ip), "%s", resolved);
                    remapped++;
                }
            }

            int is_primary = file->ss_port == client_port && strcmp(file->ss_ip, resolved) == 0;
            int is_backup = file->backup_ss_port == client_port && strcmp(file->backup_ss_ip, resolved) == 0;
            if (!is_primary && !is_backup) {
                continue;
            }
            if (!name_in_list(manifest, manifest_count, file->filename)) {
                if (is_primary) {
                    file->primary_stale = 1;
                } else {
                    file->backup_stale = 1;
                }
                missing++;
            }
            int stale = is_primary ? file->primary_stale : file->backup_stale;
            int other_port = is_primary ? file->backup_ss_port : file->ss_port;
            if (stale && other_port) {
                SyncJob *tmp = realloc(jobs, sizeof(SyncJob) * (size_t)(job_count + 1));
                if (tmp) {
                    jobs = tmp;
                    snprintf(jobs[job_count].filename, sizeof(jobs[job_count].filename), "%s", file->filename);
                    snprintf(jobs[job_count].ip, sizeof(jobs[job_count].ip), "%s",
                             is_primary ? file->backup_ss_ip : file->ss_ip);
                    jobs[job_count].port = other_port;
                    job_count++;
                }
            }
        }
    }

    int orphans = 0;
    for (int i = 0; i < manifest_count; i++) {
        if (!find_any_file(manifest[i])) {
            orphans++;
        }
    }
    pthread_mutex_unlock(&files_mutex);

    for (int i = 0; i < manifest_count; i++) {
        free(manifest[i]);
    }
    free(manifest);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg),
             "Storage Server %s: %s:%d (remapped %d, missing %d, unknown files on disk %d)",
             existing ? "re-registered" : "registered", resolved, client_port, remapped, missing, orphans);
    log_message("INFO", log_msg, resolved, client_port, "SS");
    send_response(ss_fd, "{\"status\":\"OK\",\"msg\":\"SS Registered successfully\"}");

    /* Bring this server's stale copies up to date from the other replica. */
    for (int i = 0; i < job_count; i++) {
        if (ss_alive(jobs[i].ip, jobs[i].port) &&
            sync_replica(jobs[i].filename, jobs[i].ip, jobs[i].port, resolved, client_port)) {
            set_replica_stale(jobs[i].filename, resolved, client_port, 0);
        }
    }
    free(jobs);
    save_metadata();
}

void handle_ss_heartbeat(int ss_fd, const char *request, const char *ss_ip) {
    char advertised[INET_ADDRSTRLEN] = {0};
    parse_json_string(request, "ip", advertised, sizeof(advertised));
    int client_port = parse_json_int(request, "client_port");
    if (!verify_ss_auth(request, "ss_heartbeat", "", client_port)) {
        send_error(ss_fd, "UNAUTHORIZED");
        return;
    }

    char resolved[INET_ADDRSTRLEN];
    resolve_ss_ip(advertised, ss_ip, resolved, sizeof(resolved));

    int found = 0;
    pthread_mutex_lock(&ss_mutex);
    for (int i = 0; i < ss_count; i++) {
        if (storage_servers[i].client_port == client_port && strcmp(storage_servers[i].ip, resolved) == 0) {
            storage_servers[i].last_seen = time(NULL);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&ss_mutex);

    /* After a name server restart the registry is empty; this tells the
     * storage server to register again. */
    send_response(ss_fd, found ? "{\"status\":\"OK\"}" : "{\"status\":\"ERR\",\"reason\":\"UNKNOWN_SS\"}");
}

/* A storage server reports that a file changed (WRITE or UNDO); the other
 * replica is brought up to date, or marked stale if it can't be reached. */
void handle_ss_commit(int ss_fd, const char *request, const char *ss_ip) {
    char advertised[INET_ADDRSTRLEN] = {0};
    char filename[MAX_FILENAME] = {0};
    parse_json_string(request, "ip", advertised, sizeof(advertised));
    parse_json_string(request, "filename", filename, sizeof(filename));
    int client_port = parse_json_int(request, "client_port");
    if (!dnfs_valid_filename(filename) || !verify_ss_auth(request, "ss_commit", filename, client_port)) {
        send_error(ss_fd, "UNAUTHORIZED");
        return;
    }

    char resolved[INET_ADDRSTRLEN];
    resolve_ss_ip(advertised, ss_ip, resolved, sizeof(resolved));

    char other_ip[INET_ADDRSTRLEN] = {0};
    int other_port = 0;
    pthread_mutex_lock(&files_mutex);
    FileMetadata *file = lookup_file(filename);
    if (!file) {
        pthread_mutex_unlock(&files_mutex);
        send_error(ss_fd, "FILE_NOT_FOUND");
        return;
    }
    int is_primary = file->ss_port == client_port && strcmp(file->ss_ip, resolved) == 0;
    int is_backup = file->backup_ss_port == client_port && strcmp(file->backup_ss_ip, resolved) == 0;
    if (!is_primary && !is_backup) {
        pthread_mutex_unlock(&files_mutex);
        send_error(ss_fd, "NOT_A_REPLICA");
        return;
    }
    file->last_modified = time(NULL);
    if (is_primary) {
        file->primary_stale = 0;
        snprintf(other_ip, sizeof(other_ip), "%s", file->backup_ss_ip);
        other_port = file->backup_ss_port;
    } else {
        file->backup_stale = 0;
        snprintf(other_ip, sizeof(other_ip), "%s", file->ss_ip);
        other_port = file->ss_port;
    }
    pthread_mutex_unlock(&files_mutex);

    send_response(ss_fd, "{\"status\":\"OK\"}");

    if (other_port) {
        int ok = ss_alive(other_ip, other_port) &&
                 sync_replica(filename, resolved, client_port, other_ip, other_port);
        set_replica_stale(filename, other_ip, other_port, !ok);
    }
    save_metadata();
}

/* ---------- user commands ---------- */

typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    char ip[INET_ADDRSTRLEN];
    int port;
    char backup_ip[INET_ADDRSTRLEN];
    int backup_port;
    int words;
    int chars;
    int bytes;
    time_t last_accessed;
} ViewRow;

static int compare_rows(const void *a, const void *b) {
    return strcmp(((const ViewRow *)a)->filename, ((const ViewRow *)b)->filename);
}

/* Fetches fresh counts from whichever replica is alive, without holding
 * files_mutex, and stores them back. */
static void refresh_stats(ViewRow *row) {
    const char *ip = row->ip;
    int port = row->port;
    if (!ss_alive(ip, port)) {
        if (!row->backup_port || !ss_alive(row->backup_ip, row->backup_port)) {
            return;
        }
        ip = row->backup_ip;
        port = row->backup_port;
    }
    if (!request_file_stats(ip, port, row->filename, &row->words, &row->chars, &row->bytes)) {
        return;
    }
    pthread_mutex_lock(&files_mutex);
    FileMetadata *file = lookup_file(row->filename);
    if (file) {
        file->words = row->words;
        file->chars = row->chars;
        file->bytes = row->bytes;
    }
    pthread_mutex_unlock(&files_mutex);
}

static void copy_row(ViewRow *row, const FileMetadata *file) {
    snprintf(row->filename, sizeof(row->filename), "%s", file->filename);
    snprintf(row->owner, sizeof(row->owner), "%s", file->owner);
    snprintf(row->ip, sizeof(row->ip), "%s", file->ss_ip);
    row->port = file->ss_port;
    snprintf(row->backup_ip, sizeof(row->backup_ip), "%s", file->backup_ss_ip);
    row->backup_port = file->backup_ss_port;
    row->words = file->words;
    row->chars = file->chars;
    row->bytes = file->bytes;
    row->last_accessed = file->last_accessed;
}

static void format_time(time_t t, const char *fmt, char *out, size_t size) {
    struct tm tm_info;
    if (localtime_r(&t, &tm_info)) {
        strftime(out, size, fmt, &tm_info);
    } else {
        snprintf(out, size, "unknown");
    }
}

void handle_view(int client_fd, const char *request, const char *username) {
    char flags[16] = {0};
    parse_json_string(request, "flags", flags, sizeof(flags));

    int show_all = 0;
    int show_details = 0;
    if (flags[0]) {
        if (flags[0] != '-' || !flags[1]) {
            send_error(client_fd, "INVALID_FLAGS");
            return;
        }
        for (const char *f = flags + 1; *f; f++) {
            if (*f == 'a') {
                show_all = 1;
            } else if (*f == 'l') {
                show_details = 1;
            } else {
                send_error(client_fd, "INVALID_FLAGS");
                return;
            }
        }
    }

    ViewRow *rows = NULL;
    int row_count = 0;
    pthread_mutex_lock(&files_mutex);
    for (int i = 0; i < HASH_BUCKETS; i++) {
        for (HashNode *node = file_hash_table[i]; node; node = node->next) {
            FileMetadata *file = node->file;
            if (file->pending || !(show_all || check_access(file, username, "R"))) {
                continue;
            }
            ViewRow *tmp = realloc(rows, sizeof(ViewRow) * (size_t)(row_count + 1));
            if (!tmp) {
                continue;
            }
            rows = tmp;
            copy_row(&rows[row_count++], file);
        }
    }
    pthread_mutex_unlock(&files_mutex);

    if (row_count > 1) {
        qsort(rows, (size_t)row_count, sizeof(ViewRow), compare_rows);
    }

    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\"status\":\"OK\",\"files\":[");
    for (int i = 0; i < row_count; i++) {
        if (i) {
            sb_append(&sb, ",");
        }
        if (show_details) {
            refresh_stats(&rows[i]);
            char timestamp[32];
            format_time(rows[i].last_accessed, "%Y-%m-%d %H:%M", timestamp, sizeof(timestamp));
            sb_append(&sb, "{\"filename\":");
            sb_append_json(&sb, rows[i].filename);
            sb_append(&sb, ",\"owner\":");
            sb_append_json(&sb, rows[i].owner);
            sb_appendf(&sb, ",\"words\":%d,\"chars\":%d,\"bytes\":%d,\"last_accessed\":\"%s\"}",
                       rows[i].words, rows[i].chars, rows[i].bytes, timestamp);
        } else {
            sb_append_json(&sb, rows[i].filename);
        }
    }
    sb_append(&sb, "]}");
    free(rows);

    char *response = sb_take(&sb);
    if (!response) {
        send_error(client_fd, "UNKNOWN");
        return;
    }
    send_response(client_fd, response);
    free(response);
    log_message("INFO", "VIEW command executed", "-", 0, username);
}

void handle_list(int client_fd, const char *username) {
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\"status\":\"OK\",\"users\":[");
    pthread_mutex_lock(&users_mutex);
    for (int i = 0; i < user_count; i++) {
        if (i) {
            sb_append(&sb, ",");
        }
        sb_append_json(&sb, users[i].username);
    }
    pthread_mutex_unlock(&users_mutex);
    sb_append(&sb, "]}");

    char *response = sb_take(&sb);
    if (!response) {
        send_error(client_fd, "UNKNOWN");
        return;
    }
    send_response(client_fd, response);
    free(response);
    log_message("INFO", "LIST command executed", "-", 0, username);
}

typedef struct {
    char ip[INET_ADDRSTRLEN];
    int port;
    int registry_index;
} Candidate;

/* Live storage servers in round-robin order, starting after the server
 * that received the previous file. */
static int live_candidates(Candidate *out, int *next_index) {
    time_t now = time(NULL);
    int count = 0;
    pthread_mutex_lock(&ss_mutex);
    for (int k = 0; k < ss_count; k++) {
        int i = (*next_index + k) % ss_count;
        if (now - storage_servers[i].last_seen <= ss_timeout_sec) {
            snprintf(out[count].ip, sizeof(out[count].ip), "%s", storage_servers[i].ip);
            out[count].port = storage_servers[i].client_port;
            out[count].registry_index = i;
            count++;
        }
    }
    pthread_mutex_unlock(&ss_mutex);
    return count;
}

void handle_create(int client_fd, const char *request, const char *username) {
    static int next_ss_index = 0;

    char filename[MAX_FILENAME] = {0};
    parse_json_string(request, "filename", filename, sizeof(filename));
    if (!dnfs_valid_filename(filename)) {
        send_error(client_fd, "INVALID_FILENAME");
        return;
    }

    /* Reserve the name first, so two CREATEs for it can't both proceed. */
    FileMetadata *file = calloc(1, sizeof(FileMetadata));
    if (!file) {
        send_error(client_fd, "UNKNOWN");
        return;
    }
    snprintf(file->filename, sizeof(file->filename), "%s", filename);
    snprintf(file->owner, sizeof(file->owner), "%s", username);
    snprintf(file->last_accessed_by, sizeof(file->last_accessed_by), "%s", username);
    file->created_at = file->last_modified = file->last_accessed = time(NULL);
    file->pending = 1;

    pthread_mutex_lock(&files_mutex);
    if (find_any_file(filename)) {
        pthread_mutex_unlock(&files_mutex);
        free(file);
        send_error(client_fd, "ALREADY_EXISTS");
        return;
    }
    insert_file(file);
    pthread_mutex_unlock(&files_mutex);

    Candidate candidates[MAX_STORAGE_SERVERS];
    pthread_mutex_lock(&ss_mutex);
    int start = ss_count ? next_ss_index % ss_count : 0;
    pthread_mutex_unlock(&ss_mutex);
    int count = live_candidates(candidates, &start);

    int primary = -1;
    for (int k = 0; k < count && primary < 0; k++) {
        char *resp = nm_ss_command(candidates[k].ip, candidates[k].port, "CREATE", filename,
                                   ",\"content\":\"\"", SS_CALL_TIMEOUT);
        if (resp_ok(resp)) {
            primary = k;
        }
        free(resp);
    }

    if (primary < 0) {
        pthread_mutex_lock(&files_mutex);
        remove_file(filename);
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, count == 0 ? "NO_SS_AVAILABLE" : "ALL_SS_FAILED");
        return;
    }

    pthread_mutex_lock(&ss_mutex);
    next_ss_index = ss_count ? (candidates[primary].registry_index + 1) % ss_count : 0;
    pthread_mutex_unlock(&ss_mutex);

    /* The backup goes on the next live server after the primary, so backups
     * spread round-robin too. A leftover file there is overwritten. */
    int backup = -1;
    for (int k = 1; k < count && backup < 0; k++) {
        int c = (primary + k) % count;
        char *resp = nm_ss_command(candidates[c].ip, candidates[c].port, "CREATE", filename,
                                   ",\"content\":\"\"", SS_CALL_TIMEOUT);
        int ok = resp_ok(resp);
        free(resp);
        if (!ok) {
            resp = nm_ss_command(candidates[c].ip, candidates[c].port, "PUT", filename,
                                 ",\"content\":\"\"", SS_CALL_TIMEOUT);
            ok = resp_ok(resp);
            free(resp);
        }
        if (ok) {
            backup = c;
        }
    }

    pthread_mutex_lock(&files_mutex);
    file = find_any_file(filename);
    if (file) {
        snprintf(file->ss_ip, sizeof(file->ss_ip), "%s", candidates[primary].ip);
        file->ss_port = candidates[primary].port;
        if (backup >= 0) {
            snprintf(file->backup_ss_ip, sizeof(file->backup_ss_ip), "%s", candidates[backup].ip);
            file->backup_ss_port = candidates[backup].port;
        }
        mark_file_ready(file);
    }
    pthread_mutex_unlock(&files_mutex);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "File created: %s by %s on SS %s:%d (backup %s:%d)",
             filename, username, candidates[primary].ip, candidates[primary].port,
             backup >= 0 ? candidates[backup].ip : "none", backup >= 0 ? candidates[backup].port : 0);
    log_message("INFO", log_msg, "-", 0, username);

    save_metadata();

    char response[256];
    snprintf(response, sizeof(response), "{\"status\":\"OK\",\"ss_ip\":\"%s\",\"ss_port\":%d}",
             candidates[primary].ip, candidates[primary].port);
    send_response(client_fd, response);
}

void handle_info(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    parse_json_string(request, "filename", filename, sizeof(filename));
    if (!dnfs_valid_filename(filename)) {
        send_error(client_fd, "INVALID_FILENAME");
        return;
    }

    ViewRow row;
    char last_accessed_by[MAX_USERNAME];
    time_t created_at, last_modified;
    StrBuf access;
    sb_init(&access);

    pthread_mutex_lock(&files_mutex);
    FileMetadata *file = lookup_file(filename);
    if (!file) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "FILE_NOT_FOUND");
        return;
    }
    if (!check_access(file, username, "R")) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "UNAUTHORIZED");
        return;
    }
    copy_row(&row, file);
    snprintf(last_accessed_by, sizeof(last_accessed_by), "%s", file->last_accessed_by);
    created_at = file->created_at;
    last_modified = file->last_modified;

    sb_append(&access, "[{\"user\":");
    sb_append_json(&access, file->owner);
    sb_append(&access, ",\"mode\":\"RW\"}");
    for (AccessEntry *entry = file->access_list; entry; entry = entry->next) {
        sb_append(&access, ",{\"user\":");
        sb_append_json(&access, entry->username);
        sb_append(&access, ",\"mode\":");
        sb_append_json(&access, entry->mode);
        sb_append(&access, "}");
    }
    sb_append(&access, "]");
    pthread_mutex_unlock(&files_mutex);

    refresh_stats(&row);

    char created_str[64], modified_str[64], accessed_str[64];
    format_time(created_at, "%Y-%m-%d %H:%M:%S", created_str, sizeof(created_str));
    format_time(last_modified, "%Y-%m-%d %H:%M:%S", modified_str, sizeof(modified_str));
    format_time(row.last_accessed, "%Y-%m-%d %H:%M:%S", accessed_str, sizeof(accessed_str));

    char *access_json = sb_take(&access);
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\"status\":\"OK\",\"filename\":");
    sb_append_json(&sb, row.filename);
    sb_append(&sb, ",\"owner\":");
    sb_append_json(&sb, row.owner);
    sb_appendf(&sb, ",\"created_at\":\"%s\",\"last_modified\":\"%s\",\"last_accessed\":\"%s\",\"last_accessed_by\":",
               created_str, modified_str, accessed_str);
    sb_append_json(&sb, last_accessed_by);
    sb_appendf(&sb, ",\"words\":%d,\"chars\":%d,\"bytes\":%d,\"access\":%s,\"ss_ip\":\"%s\",\"ss_port\":%d"
                    ",\"backup_ss_ip\":\"%s\",\"backup_ss_port\":%d}",
               row.words, row.chars, row.bytes, access_json ? access_json : "[]", row.ip, row.port,
               row.backup_ip, row.backup_port);
    free(access_json);

    char *response = sb_take(&sb);
    if (!response) {
        send_error(client_fd, "UNKNOWN");
        return;
    }
    send_response(client_fd, response);
    free(response);
    log_message("INFO", "INFO command executed", "-", 0, username);
}

void handle_addaccess(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    char target[MAX_USERNAME] = {0};
    char mode[3] = {0};

    parse_json_string(request, "filename", filename, sizeof(filename));
    parse_json_string(request, "target", target, sizeof(target));
    parse_json_string(request, "mode", mode, sizeof(mode));

    if (!dnfs_valid_filename(filename) || !dnfs_valid_username(target) ||
        (strcmp(mode, "R") != 0 && strcmp(mode, "W") != 0)) {
        send_error(client_fd, "BAD_REQUEST");
        return;
    }

    pthread_mutex_lock(&files_mutex);
    FileMetadata *file = lookup_file(filename);
    if (!file) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "FILE_NOT_FOUND");
        return;
    }
    if (strcmp(file->owner, username) != 0) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "UNAUTHORIZED");
        return;
    }
    if (strcmp(file->owner, target) == 0) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "ALREADY_OWNER");
        return;
    }

    pthread_mutex_lock(&users_mutex);
    int known = user_index(target) >= 0;
    pthread_mutex_unlock(&users_mutex);
    if (!known) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "USER_NOT_FOUND");
        return;
    }

    AccessEntry *entry = file->access_list;
    while (entry && strcmp(entry->username, target) != 0) {
        entry = entry->next;
    }
    if (!entry) {
        entry = calloc(1, sizeof(AccessEntry));
        if (!entry) {
            pthread_mutex_unlock(&files_mutex);
            send_error(client_fd, "UNKNOWN");
            return;
        }
        snprintf(entry->username, sizeof(entry->username), "%s", target);
        entry->next = file->access_list;
        file->access_list = entry;
    }
    snprintf(entry->mode, sizeof(entry->mode), "%s", mode);
    pthread_mutex_unlock(&files_mutex);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "Access set: %s for %s on %s", mode, target, filename);
    log_message("INFO", log_msg, "-", 0, username);

    save_metadata();
    send_response(client_fd, "{\"status\":\"OK\",\"msg\":\"Access granted\"}");
}

void handle_remaccess(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    char target[MAX_USERNAME] = {0};

    parse_json_string(request, "filename", filename, sizeof(filename));
    parse_json_string(request, "target", target, sizeof(target));
    if (!dnfs_valid_filename(filename) || !dnfs_valid_username(target)) {
        send_error(client_fd, "BAD_REQUEST");
        return;
    }

    pthread_mutex_lock(&files_mutex);
    FileMetadata *file = lookup_file(filename);
    if (!file) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "FILE_NOT_FOUND");
        return;
    }
    if (strcmp(file->owner, username) != 0) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "UNAUTHORIZED");
        return;
    }

    AccessEntry **indirect = &file->access_list;
    while (*indirect && strcmp((*indirect)->username, target) != 0) {
        indirect = &(*indirect)->next;
    }
    if (!*indirect) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "ACCESS_NOT_FOUND");
        return;
    }
    AccessEntry *to_free = *indirect;
    *indirect = to_free->next;
    free(to_free);
    pthread_mutex_unlock(&files_mutex);

    save_metadata();
    send_response(client_fd, "{\"status\":\"OK\",\"msg\":\"Access removed\"}");
}

/* READ, WRITE, STREAM and UNDO: check access, then hand the client a
 * storage server address plus a signed ticket that server will accept. */
void handle_file_operation(int client_fd, const char *request, const char *username) {
    char cmd[64] = {0};
    char filename[MAX_FILENAME] = {0};
    parse_json_string(request, "cmd", cmd, sizeof(cmd));
    parse_json_string(request, "filename", filename, sizeof(filename));
    if (!dnfs_valid_filename(filename)) {
        send_error(client_fd, "INVALID_FILENAME");
        return;
    }

    pthread_mutex_lock(&files_mutex);
    FileMetadata *file = lookup_file(filename);
    if (!file) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "FILE_NOT_FOUND");
        return;
    }
    const char *required_mode = (strcmp(cmd, "READ") == 0 || strcmp(cmd, "STREAM") == 0) ? "R" : "W";
    if (!check_access(file, username, required_mode)) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "UNAUTHORIZED");
        return;
    }
    file->last_accessed = time(NULL);
    snprintf(file->last_accessed_by, sizeof(file->last_accessed_by), "%s", username);
    pthread_mutex_unlock(&files_mutex);

    Route route;
    if (!choose_replica(filename, &route)) {
        send_error(client_fd, "SS_UNAVAILABLE");
        return;
    }

    long expires = (long)time(NULL) + DNFS_TICKET_TTL;
    char ticket[DNFS_HEX_LEN + 1];
    dnfs_make_ticket(NM_SECRET, username, filename, cmd, expires, ticket);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "%s operation: %s by %s (SS: %s:%d%s)",
             cmd, filename, username, route.ip, route.port, route.is_backup ? " [BACKUP]" : "");
    log_message("INFO", log_msg, "-", 0, username);

    save_metadata();

    char response[512];
    snprintf(response, sizeof(response),
             "{\"status\":\"OK\",\"ss_ip\":\"%s\",\"ss_port\":%d,\"user\":\"%s\",\"expires\":%ld,\"ticket\":\"%s\"}",
             route.ip, route.port, username, expires, ticket);
    send_response(client_fd, response);
}

/* The primary must confirm the delete before the name is forgotten, so a
 * file is never left on disk under a name the NM no longer knows. */
void handle_delete(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    parse_json_string(request, "filename", filename, sizeof(filename));
    if (!dnfs_valid_filename(filename)) {
        send_error(client_fd, "INVALID_FILENAME");
        return;
    }

    char ss_ip[INET_ADDRSTRLEN], backup_ip[INET_ADDRSTRLEN];
    int ss_port, backup_port;
    pthread_mutex_lock(&files_mutex);
    FileMetadata *file = lookup_file(filename);
    if (!file) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "FILE_NOT_FOUND");
        return;
    }
    if (strcmp(file->owner, username) != 0) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "UNAUTHORIZED");
        return;
    }
    snprintf(ss_ip, sizeof(ss_ip), "%s", file->ss_ip);
    snprintf(backup_ip, sizeof(backup_ip), "%s", file->backup_ss_ip);
    ss_port = file->ss_port;
    backup_port = file->backup_ss_port;
    pthread_mutex_unlock(&files_mutex);

    char *resp = ss_alive(ss_ip, ss_port) ? nm_ss_command(ss_ip, ss_port, "DELETE", filename, NULL, SS_CALL_TIMEOUT) : NULL;
    if (!resp_ok(resp)) {
        char reason[64];
        resp_reason(resp, reason, sizeof(reason));
        if (strcmp(reason, "FILE_NOT_FOUND") != 0) {
            free(resp);
            send_error(client_fd, reason);
            return;
        }
    }
    free(resp);

    if (backup_port) {
        resp = nm_ss_command(backup_ip, backup_port, "DELETE", filename, NULL, SS_CALL_TIMEOUT);
        if (!resp_ok(resp)) {
            char log_msg[512];
            snprintf(log_msg, sizeof(log_msg), "Backup copy of %s on %s:%d could not be deleted",
                     filename, backup_ip, backup_port);
            log_message("ERROR", log_msg, backup_ip, backup_port, username);
        }
        free(resp);
    }

    pthread_mutex_lock(&files_mutex);
    remove_file(filename);
    pthread_mutex_unlock(&files_mutex);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "File deleted: %s by %s", filename, username);
    log_message("INFO", log_msg, "-", 0, username);

    save_metadata();
    send_response(client_fd, "{\"status\":\"OK\",\"msg\":\"File deleted\"}");
}

static int remove_entry(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    (void)typeflag;
    (void)ftwbuf;
    return remove(path);
}

/* Runs a script in a child process with a wall-clock timeout, CPU, memory
 * and file-size limits, an empty working directory and a minimal
 * environment. It still runs as the name server's user. */
static int run_sandboxed(const char *script, StrBuf *output, int *exit_code, int *timed_out, int *truncated) {
    int timeout = dnfs_env_int("DNFS_EXEC_TIMEOUT", 10);
    char *const envp[] = { "PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C", NULL };

    char sandbox_dir[] = "/tmp/dnfs-exec-XXXXXX";
    if (!mkdtemp(sandbox_dir)) {
        return 0;
    }
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        rmdir(sandbox_dir);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        rmdir(sandbox_dir);
        return 0;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        if (chdir(sandbox_dir) != 0) {
            _exit(126);
        }
        struct rlimit cpu = { (rlim_t)timeout, (rlim_t)timeout };
        struct rlimit mem = { 512UL << 20, 512UL << 20 };
        struct rlimit fsize = { 1UL << 20, 1UL << 20 };
        setrlimit(RLIMIT_CPU, &cpu);
        setrlimit(RLIMIT_AS, &mem);
        setrlimit(RLIMIT_FSIZE, &fsize);
        execle("/bin/sh", "sh", "-c", script, (char *)NULL, envp);
        _exit(127);
    }

    close(pipefd[1]);
    setpgid(pid, pid);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    char chunk[4096];
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        long remaining = (long)timeout * 1000 - elapsed_ms;
        if (remaining <= 0) {
            *timed_out = 1;
            break;
        }
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int ready = poll(&pfd, 1, (int)remaining);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready == 0) {
            *timed_out = 1;
            break;
        }
        ssize_t n = read(pipefd[0], chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        if (output->len + (size_t)n > EXEC_OUTPUT_CAP) {
            sb_appendn(output, chunk, EXEC_OUTPUT_CAP - output->len);
            *truncated = 1;
            break;
        }
        sb_appendn(output, chunk, (size_t)n);
    }
    close(pipefd[0]);

    /* Kills the script and anything it left running in the background. */
    kill(-pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    nftw(sandbox_dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);

    if (*timed_out || *truncated) {
        *exit_code = -1;
    } else if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    } else {
        *exit_code = 128 + WTERMSIG(status);
    }
    return 1;
}

/* EXEC runs on the name server: the file is fetched from its storage server
 * and run as a shell script inside run_sandboxed(). */
void handle_exec(int client_fd, const char *request, const char *username) {
    char filename[MAX_FILENAME] = {0};
    parse_json_string(request, "filename", filename, sizeof(filename));
    if (!dnfs_valid_filename(filename)) {
        send_error(client_fd, "INVALID_FILENAME");
        return;
    }

    pthread_mutex_lock(&files_mutex);
    FileMetadata *file = lookup_file(filename);
    if (!file) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "FILE_NOT_FOUND");
        return;
    }
    if (!check_access(file, username, "R")) {
        pthread_mutex_unlock(&files_mutex);
        send_error(client_fd, "UNAUTHORIZED");
        return;
    }
    file->last_accessed = time(NULL);
    snprintf(file->last_accessed_by, sizeof(file->last_accessed_by), "%s", username);
    pthread_mutex_unlock(&files_mutex);

    Route route;
    if (!choose_replica(filename, &route)) {
        send_error(client_fd, "SS_UNAVAILABLE");
        return;
    }

    char *resp = nm_ss_command(route.ip, route.port, "READ", filename, NULL, SS_CALL_TIMEOUT);
    if (!resp_ok(resp)) {
        char reason[64];
        resp_reason(resp, reason, sizeof(reason));
        free(resp);
        send_error(client_fd, reason);
        return;
    }
    char *script = json_get_str_alloc(resp, "content");
    free(resp);
    if (!script || !script[0]) {
        free(script);
        send_error(client_fd, "EMPTY_FILE");
        return;
    }

    StrBuf output;
    sb_init(&output);
    int exit_code = 0, timed_out = 0, truncated = 0;
    int ran = run_sandboxed(script, &output, &exit_code, &timed_out, &truncated);
    free(script);
    char *output_text = sb_take(&output);
    if (!ran || !output_text) {
        free(output_text);
        send_error(client_fd, "EXEC_FAILED");
        return;
    }

    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\"status\":\"OK\",\"output\":");
    sb_append_json(&sb, output_text);
    sb_appendf(&sb, ",\"exit_code\":%d,\"timed_out\":%s,\"truncated\":%s}",
               exit_code, timed_out ? "true" : "false", truncated ? "true" : "false");
    free(output_text);
    char *response = sb_take(&sb);
    if (!response) {
        send_error(client_fd, "UNKNOWN");
        return;
    }
    send_response(client_fd, response);
    free(response);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg), "EXEC: %s by %s (exit_code=%d%s)",
             filename, username, exit_code, timed_out ? ", timed out" : "");
    log_message("INFO", log_msg, "-", 0, username);

    save_metadata();
}
