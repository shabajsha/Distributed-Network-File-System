#include "nm_common.h"
#include "nm_cache.h"
#include "nm_logging.h"
#include "nm_metadata.h"
#include "nm_network.h"

#include <signal.h>

char BASE_DIR[256] = "./name_server";
char LOG_DIR[1024];
char METADATA_FILE[1024];
char NM_SECRET[SECRET_LEN];
int ss_timeout_sec = 15;

User users[MAX_USERS];
int user_count = 0;
Session sessions[MAX_SESSIONS];
StorageServer storage_servers[MAX_STORAGE_SERVERS];
int ss_count = 0;
HashNode *file_hash_table[HASH_BUCKETS];
LRUCache file_cache;
pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ss_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t files_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t save_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t replication_mutex = PTHREAD_MUTEX_INITIALIZER;
FILE *log_file = NULL;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static int active_threads = 0;
static pthread_mutex_t threads_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_name_server(void) {
    snprintf(LOG_DIR, sizeof(LOG_DIR), "%s/logs", BASE_DIR);
    snprintf(METADATA_FILE, sizeof(METADATA_FILE), "%s/metadata_store.json", BASE_DIR);

    mkdir(BASE_DIR, 0755);
    mkdir(LOG_DIR, 0755);

    char log_filename[1100];
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    snprintf(log_filename, sizeof(log_filename), "%s/nm_%04d%02d%02d_%02d%02d%02d.log",
             LOG_DIR, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    log_file = fopen(log_filename, "w");
    if (!log_file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }

    if (!dnfs_load_secret(NM_SECRET, sizeof(NM_SECRET), 1)) {
        fprintf(stderr, "Could not read or create the cluster secret (%s or $DNFS_SECRET)\n", DNFS_SECRET_FILE);
        exit(EXIT_FAILURE);
    }
    ss_timeout_sec = dnfs_env_int("DNFS_SS_TIMEOUT", 15);

    memset(file_hash_table, 0, sizeof(file_hash_table));
    init_cache();
    memset(users, 0, sizeof(users));
    memset(sessions, 0, sizeof(sessions));
    memset(storage_servers, 0, sizeof(storage_servers));
    load_metadata();

    printf("Name Server initialized with LRU cache (size: %d)\n", CACHE_SIZE);
}

static void *connection_thread(void *arg) {
    handle_connection(arg);
    pthread_mutex_lock(&threads_mutex);
    active_threads--;
    pthread_mutex_unlock(&threads_mutex);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    init_name_server();

    int port = dnfs_nm_port();
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((unsigned short)port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 128) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Name Server started on port %d\n", port);
    log_message("INFO", "Name Server started", "0.0.0.0", port, "system");

    while (1) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int new_socket = accept(server_fd, (struct sockaddr *)&peer, &peer_len);
        if (new_socket < 0) {
            perror("Accept failed");
            continue;
        }

        pthread_mutex_lock(&threads_mutex);
        int busy = active_threads >= MAX_CONNECTION_THREADS;
        if (!busy) {
            active_threads++;
        }
        pthread_mutex_unlock(&threads_mutex);
        if (busy) {
            dnfs_send_line(new_socket, "{\"status\":\"ERR\",\"reason\":\"SERVER_BUSY\"}");
            close(new_socket);
            continue;
        }

        int *pclient = malloc(sizeof(int));
        pthread_t thread_id;
        if (!pclient) {
            close(new_socket);
        } else {
            *pclient = new_socket;
            if (pthread_create(&thread_id, NULL, connection_thread, pclient) == 0) {
                pthread_detach(thread_id);
                continue;
            }
            perror("Thread creation failed");
            free(pclient);
            close(new_socket);
        }
        pthread_mutex_lock(&threads_mutex);
        active_threads--;
        pthread_mutex_unlock(&threads_mutex);
    }

    return 0;
}
