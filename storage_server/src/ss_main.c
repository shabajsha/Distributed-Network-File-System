// Main entry point for the Storage Server
// All functionality has been modularized into separate files

#include "ss_common.h"
#include "ss_file_ops.h"
#include "ss_locking.h"
#include "ss_utils.h"
#include "ss_network.h"
#include <pthread.h>
#include <signal.h>

static int active_threads = 0;
static pthread_mutex_t threads_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *counted_client_thread(void *arg) {
    client_thread(arg);
    pthread_mutex_lock(&threads_mutex);
    active_threads--;
    pthread_mutex_unlock(&threads_mutex);
    return NULL;
}

int main(int argc, char *argv[]) {
    // A client that disconnects mid-reply must not kill the server.
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    // Parse command line arguments
    // Usage: ./ss [port] [nm_ip] [advertise_ip]
    if (argc > 1) {
        int port = atoi(argv[1]);
        if (port > 0 && port < 65536) {
            CLIENT_PORT = port;
        } else {
            fprintf(stderr, "Invalid port number. Using default port %d\n", CLIENT_PORT);
        }
    }

    if (argc > 2) {
        snprintf(NM_IP, sizeof(NM_IP), "%s", argv[2]);
        printf("[SS] Connecting to Name Server at %s:%d\n", NM_IP, dnfs_nm_port());
    } else {
        printf("[SS] Usage: %s [port] [nm_ip] [advertise_ip]\n", argv[0]);
        printf("[SS] Using default Name Server IP: %s\n", NM_IP);
    }

    if (argc > 3) {
        snprintf(ADVERTISE_IP, sizeof(ADVERTISE_IP), "%s", argv[3]);
        printf("[SS] Will advertise IP: %s\n", ADVERTISE_IP);
    }

    if (!dnfs_load_secret(SS_SECRET, sizeof(SS_SECRET), 0)) {
        fprintf(stderr, "[SS] No cluster secret found. Start the Name Server first from this directory "
                        "(it creates ./%s), copy that file here, or set DNFS_SECRET.\n", DNFS_SECRET_FILE);
        return 1;
    }

    // Initialize subsystems
    ensure_directories();
    init_logging();
    locking_init();

    log_event("INFO", "0.0.0.0", CLIENT_PORT, "-", "START", "Storage server starting");

    // Create server socket
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size;

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("[SS] Socket creation failed");
        return 1;
    }

    // Set SO_REUSEADDR to allow immediate reuse of the port
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[SS] setsockopt failed");
        close(server_sock);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(CLIENT_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[SS] Bind failed");
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, 128) < 0) {
        perror("[SS] Listen failed");
        close(server_sock);
        return 1;
    }

    printf("[SS] Storage Server ready on port %d\n", CLIENT_PORT);

    // Registers with the name server, then keeps sending heartbeats
    pthread_t hb_tid;
    if (pthread_create(&hb_tid, NULL, heartbeat_thread, NULL) != 0) {
        perror("[SS] Heartbeat thread failed");
        return 1;
    }
    pthread_detach(hb_tid);

    // Main server loop - accept and handle client connections
    while (1) {
        addr_size = sizeof(client_addr);
        client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_size);
        if (client_sock < 0) {
            continue;
        }

        pthread_mutex_lock(&threads_mutex);
        int busy = active_threads >= MAX_CONNECTION_THREADS;
        if (!busy) {
            active_threads++;
        }
        pthread_mutex_unlock(&threads_mutex);
        if (busy) {
            dnfs_send_line(client_sock, "{ \"status\":\"ERR\", \"reason\":\"SERVER_BUSY\" }");
            close(client_sock);
            continue;
        }

        // Create thread argument
        ClientThreadArg *arg = (ClientThreadArg *)malloc(sizeof(ClientThreadArg));
        pthread_t tid;
        if (arg) {
            arg->socket_fd = client_sock;
            memcpy(&arg->addr, &client_addr, sizeof(client_addr));
            if (pthread_create(&tid, NULL, counted_client_thread, arg) == 0) {
                pthread_detach(tid);
                continue;
            }
            free(arg);
        }
        close(client_sock);
        pthread_mutex_lock(&threads_mutex);
        active_threads--;
        pthread_mutex_unlock(&threads_mutex);
    }

    return 0;
}
