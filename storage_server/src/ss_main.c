// Main entry point for the Storage Server
// All functionality has been modularized into separate files

#include "ss_common.h"
#include "ss_file_ops.h"
#include "ss_locking.h"
#include "ss_utils.h"
#include "ss_network.h"
#include <pthread.h>

int main(int argc, char *argv[]) {
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
        strncpy(NM_IP, argv[2], INET_ADDRSTRLEN - 1);
        NM_IP[INET_ADDRSTRLEN - 1] = '\0';
        printf("[SS] Connecting to Name Server at %s:%d\n", NM_IP, NM_PORT);
    } else {
        printf("[SS] Usage: %s [port] [nm_ip] [advertise_ip]\n", argv[0]);
        printf("[SS] Using default Name Server IP: %s\n", NM_IP);
    }
    
    if (argc > 3) {
        strncpy(ADVERTISE_IP, argv[3], INET_ADDRSTRLEN - 1);
        ADVERTISE_IP[INET_ADDRSTRLEN - 1] = '\0';
        printf("[SS] Will advertise IP: %s\n", ADVERTISE_IP);
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

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(CLIENT_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[SS] Bind failed");
        close(server_sock);
        return 1;
    }
    
    if (listen(server_sock, 5) < 0) {
        perror("[SS] Listen failed");
        close(server_sock);
        return 1;
    }

    printf("[SS] Storage Server ready on port %d\n", CLIENT_PORT);
    
    // Register with name server
    register_with_nm();

    // Main server loop - accept and handle client connections
    while (1) {
        addr_size = sizeof(client_addr);
        client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_size);
        if (client_sock < 0) {
            continue;
        }

        // Create thread argument
        ClientThreadArg *arg = (ClientThreadArg *)malloc(sizeof(ClientThreadArg));
        if (!arg) {
            close(client_sock);
            continue;
        }
        arg->socket_fd = client_sock;
        memcpy(&arg->addr, &client_addr, sizeof(client_addr));

        // Spawn thread to handle client
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
            free(arg);
            close(client_sock);
            continue;
        }
        pthread_detach(tid);
    }

    return 0;
}
