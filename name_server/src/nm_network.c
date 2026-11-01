#include "nm_network.h"
#include "nm_handlers.h"
#include "nm_logging.h"
#include "nm_metadata.h"

/* Resolves a login token to its username. */
static int session_user(const char *token, char *username, size_t size) {
    if (strlen(token) != DNFS_HEX_LEN) {
        return 0;
    }
    int found = 0;
    pthread_mutex_lock(&users_mutex);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].token[0] && strcmp(sessions[i].token, token) == 0) {
            snprintf(username, size, "%s", sessions[i].username);
            sessions[i].last_used = time(NULL);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&users_mutex);
    return found;
}

void *handle_connection(void *arg) {
    int socket_fd = *(int *)arg;
    free(arg);
    dnfs_set_timeout(socket_fd, 30);

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    char client_ip[INET_ADDRSTRLEN] = "unknown";
    int client_port = 0;
    if (getpeername(socket_fd, (struct sockaddr *)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        client_port = ntohs(addr.sin_port);
    }

    char *request = read_request(socket_fd);
    if (!request) {
        close(socket_fd);
        return NULL;
    }

    char cmd[64] = {0};
    parse_json_string(request, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "register_client") == 0) {
        handle_register_client(socket_fd, request, client_ip);
    } else if (strcmp(cmd, "register_ss") == 0) {
        handle_register_ss(socket_fd, request, client_ip);
    } else if (strcmp(cmd, "ss_heartbeat") == 0) {
        handle_ss_heartbeat(socket_fd, request, client_ip);
    } else if (strcmp(cmd, "ss_commit") == 0) {
        handle_ss_commit(socket_fd, request, client_ip);
    } else {
        /* Every user command must carry the token from register_client;
         * any "username" field in the request is ignored. */
        char token[DNFS_HEX_LEN + 1] = {0};
        char username[MAX_USERNAME] = {0};
        parse_json_string(request, "token", token, sizeof(token));

        if (!session_user(token, username, sizeof(username))) {
            send_error(socket_fd, "UNAUTHENTICATED");
        } else {
            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg), "Received command %s", cmd);
            log_message("INFO", log_msg, client_ip, client_port, username);

            if (strcmp(cmd, "VIEW") == 0) {
                handle_view(socket_fd, request, username);
            } else if (strcmp(cmd, "LIST") == 0) {
                handle_list(socket_fd, username);
            } else if (strcmp(cmd, "CREATE") == 0) {
                handle_create(socket_fd, request, username);
            } else if (strcmp(cmd, "INFO") == 0) {
                handle_info(socket_fd, request, username);
            } else if (strcmp(cmd, "ADDACCESS") == 0) {
                handle_addaccess(socket_fd, request, username);
            } else if (strcmp(cmd, "REMACCESS") == 0) {
                handle_remaccess(socket_fd, request, username);
            } else if (strcmp(cmd, "DELETE") == 0) {
                handle_delete(socket_fd, request, username);
            } else if (strcmp(cmd, "READ") == 0 || strcmp(cmd, "WRITE") == 0 ||
                       strcmp(cmd, "STREAM") == 0 || strcmp(cmd, "UNDO") == 0) {
                handle_file_operation(socket_fd, request, username);
            } else if (strcmp(cmd, "EXEC") == 0) {
                handle_exec(socket_fd, request, username);
            } else {
                send_error(socket_fd, "UNKNOWN_COMMAND");
            }
        }
    }

    free(request);
    close(socket_fd);
    return NULL;
}

void send_response(int fd, const char *response) {
    if (response) {
        dnfs_send_line(fd, response);
    }
}

void send_error(int fd, const char *reason) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"status\":\"ERR\",\"reason\":\"%s\"}", reason);
    send_response(fd, buf);
}

char *read_request(int fd) {
    return dnfs_recv_line(fd);
}

/* Sends one request to a storage server and returns its one-line reply. */
char *ss_call(const char *ip, int port, const char *request, int timeout_sec) {
    int fd = dnfs_connect(ip, port, timeout_sec);
    if (fd < 0) {
        return NULL;
    }
    char *response = NULL;
    if (dnfs_send_line(fd, request) == 0) {
        response = dnfs_recv_line(fd);
    }
    close(fd);
    return response;
}
