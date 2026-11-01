//AI code starts
#include "client_network.h"
#include "client_utils.h"

int conn_open(Conn *conn, const char *ip, int port) {
    conn->fd = dnfs_connect(ip, port, 30);
    if (conn->fd < 0) {
        return 0;
    }
    line_reader_init(&conn->reader, conn->fd);
    return 1;
}

int conn_send(Conn *conn, const char *message) {
    return dnfs_send_line(conn->fd, message) == 0;
}

/* Returns the next reply line (caller frees), or NULL if the server closed
 * the connection or stopped answering. */
char *conn_recv(Conn *conn) {
    return line_reader_next(&conn->reader);
}

void conn_close(Conn *conn) {
    line_reader_free(&conn->reader);
    if (conn->fd >= 0) {
        close(conn->fd);
    }
    conn->fd = -1;
}

/* Sends one request on a fresh connection to the name server and returns
 * its reply. */
static char *nm_request(const char *request) {
    Conn conn;
    if (!conn_open(&conn, NM_IP, dnfs_nm_port())) {
        printf("Error: Could not connect to Name Server at %s:%d\n", NM_IP, dnfs_nm_port());
        return NULL;
    }
    char *response = NULL;
    if (conn_send(&conn, request)) {
        response = conn_recv(&conn);
    }
    conn_close(&conn);
    if (!response) {
        printf("Error: No response from Name Server\n");
    }
    return response;
}

/* Logs in with the stored username and password and keeps the token. */
int login(void) {
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\"cmd\":\"register_client\",\"username\":");
    sb_append_json(&sb, current_username);
    sb_append(&sb, ",\"password\":");
    sb_append_json(&sb, current_password);
    sb_append(&sb, "}");
    char *request = sb_take(&sb);
    if (!request) {
        return 0;
    }

    char *response = nm_request(request);
    free(request);
    if (!response) {
        return 0;
    }
    int ok = response_is_ok(response) &&
             json_get_str(response, "token", session_token, sizeof(session_token));
    if (!ok) {
        print_error(response, "Login failed");
    }
    free(response);
    return ok;
}

/* Sends {"cmd":cmd,"token":...<fields>} to the name server. If the name
 * server restarted and forgot the session, logs in again once and retries. */
char *nm_call(const char *cmd, const char *fields) {
    for (int attempt = 0; attempt < 2; attempt++) {
        StrBuf sb;
        sb_init(&sb);
        sb_appendf(&sb, "{\"cmd\":\"%s\",\"token\":\"%s\"", cmd, session_token);
        sb_append(&sb, fields);
        sb_append(&sb, "}");
        char *request = sb_take(&sb);
        if (!request) {
            return NULL;
        }

        char *response = nm_request(request);
        free(request);
        if (!response) {
            return NULL;
        }

        char reason[64] = {0};
        json_get_str(response, "reason", reason, sizeof(reason));
        if (attempt == 0 && strcmp(reason, "UNAUTHENTICATED") == 0) {
            free(response);
            if (!login()) {
                return NULL;
            }
            continue;
        }
        return response;
    }
    return NULL;
}
//AI code ends
