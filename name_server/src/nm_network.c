#include "nm_network.h"
#include "nm_handlers.h"
#include "nm_logging.h"
#include "nm_metadata.h"

void *handle_connection(void *arg) {
    int socket_fd = *(int *)arg;
    free(arg);

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(socket_fd, (struct sockaddr *)&addr, &addr_len);
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, client_ip, INET_ADDRSTRLEN);

    char *request = read_request(socket_fd);
    if (!request) {
        close(socket_fd);
        return NULL;
    }

    char cmd[64] = {0};
    parse_json_string(request, "cmd", cmd, sizeof(cmd));

    log_message("INFO", "Received command", client_ip, ntohs(addr.sin_port), cmd);

    if (strcmp(cmd, "register_client") == 0) {
        handle_register_client(socket_fd, request, client_ip);
    } else if (strcmp(cmd, "register_ss") == 0) {
        handle_register_ss(socket_fd, request, client_ip);
    } else {
        char username[MAX_USERNAME] = {0};
        parse_json_string(request, "username", username, sizeof(username));

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
            send_response(socket_fd, "{\"status\":\"ERR\",\"reason\":\"UNKNOWN_COMMAND\"}");
        }
    }

    free(request);
    close(socket_fd);
    return NULL;
}

void send_response(int fd, const char *response) {
    if (!response) {
        return;
    }

    int len = (int)strlen(response);
    send(fd, response, len, 0);
}

char *read_request(int fd) {
    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        return NULL;
    }

    int bytes_read = recv(fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read <= 0) {
        free(buffer);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    return buffer;
}
