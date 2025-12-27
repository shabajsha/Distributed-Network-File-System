#include "nm_common.h"
#include "nm_cache.h"
#include "nm_logging.h"
#include "nm_metadata.h"
#include "nm_network.h"

char BASE_DIR[1024] = "./name_server";
char LOG_DIR[1024];
char METADATA_FILE[1024];

Client clients[MAX_CLIENTS];
StorageServer storage_servers[MAX_STORAGE_SERVERS];
HashNode *file_hash_table[MAX_FILES];
LRUCache file_cache;
int client_count = 0;
int ss_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ss_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t files_mutex = PTHREAD_MUTEX_INITIALIZER;
FILE *log_file = NULL;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_name_server(void) {
    snprintf(LOG_DIR, sizeof(LOG_DIR), "%s/logs", BASE_DIR);
    snprintf(METADATA_FILE, sizeof(METADATA_FILE), "%s/metadata_store.json", BASE_DIR);

    mkdir(BASE_DIR, 0755);
    mkdir(LOG_DIR, 0755);

    char log_filename[1024];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(log_filename, sizeof(log_filename), "%s/nm_%04d%02d%02d_%02d%02d%02d.log",
             LOG_DIR, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    log_file = fopen(log_filename, "w");
    if (!log_file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    memset(file_hash_table, 0, sizeof(file_hash_table));
    init_cache();
    memset(clients, 0, sizeof(clients));
    memset(storage_servers, 0, sizeof(storage_servers));
    load_metadata();

    printf("Name Server initialized with LRU cache (size: %d)\n", CACHE_SIZE);
}

int main(void) {
    init_name_server();

    int server_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(NM_PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Name Server started on port %d\n", NM_PORT);
    log_message("INFO", "Name Server started", "127.0.0.1", NM_PORT, "system");

    while (1) {
        int new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
        if (new_socket < 0) {
            perror("Accept failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &address.sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("New connection from %s:%d\n", client_ip, ntohs(address.sin_port));

        pthread_t thread_id;
        int *pclient = malloc(sizeof(int));
        if (!pclient) {
            close(new_socket);
            continue;
        }
        *pclient = new_socket;

        if (pthread_create(&thread_id, NULL, handle_connection, pclient) != 0) {
            perror("Thread creation failed");
            free(pclient);
            close(new_socket);
            continue;
        }
        pthread_detach(thread_id);
    }

    return 0;
}
