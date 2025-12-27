#ifndef NM_COMMON_H
#define NM_COMMON_H

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NM_PORT 9000
#define MAX_CLIENTS 100
#define MAX_STORAGE_SERVERS 10
#define MAX_FILES 1000
#define MAX_USERS 100
#define BUFFER_SIZE 8192
#define MAX_FILENAME 256
#define MAX_USERNAME 64
#define CACHE_SIZE 50

/* Forward declarations */
typedef struct FileMetadata FileMetadata;

typedef struct CacheNode {
    char filename[MAX_FILENAME];
    FileMetadata *file;
    struct CacheNode *prev;
    struct CacheNode *next;
    time_t last_access;
} CacheNode;

typedef struct {
    CacheNode *head;
    CacheNode *tail;
    int size;
    pthread_mutex_t mutex;
} LRUCache;

typedef struct {
    char username[MAX_USERNAME];
    int socket_fd;
    char ip[INET_ADDRSTRLEN];
    int active;
    time_t connected_at;
} Client;

typedef struct {
    char ip[INET_ADDRSTRLEN];
    int nm_port;
    int client_port;
    int socket_fd;
    int active;
    char **files;
    int file_count;
    time_t connected_at;
} StorageServer;

typedef struct AccessEntry {
    char username[MAX_USERNAME];
    char mode[3];
    struct AccessEntry *next;
} AccessEntry;

struct FileMetadata {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    char backup_ss_ip[INET_ADDRSTRLEN];
    int backup_ss_port;
    AccessEntry *access_list;
    int active;
    time_t created_at;
    time_t last_modified;
    time_t last_accessed;
    char last_accessed_by[MAX_USERNAME];
    int words;
    int chars;
    int bytes;
};

typedef struct HashNode {
    char key[MAX_FILENAME];
    FileMetadata *file;
    struct HashNode *next;
} HashNode;

extern char BASE_DIR[1024];
extern char LOG_DIR[1024];
extern char METADATA_FILE[1024];

extern Client clients[MAX_CLIENTS];
extern StorageServer storage_servers[MAX_STORAGE_SERVERS];
extern HashNode *file_hash_table[MAX_FILES];
extern LRUCache file_cache;
extern int client_count;
extern int ss_count;
extern pthread_mutex_t clients_mutex;
extern pthread_mutex_t ss_mutex;
extern pthread_mutex_t files_mutex;
extern FILE *log_file;
extern pthread_mutex_t log_mutex;

void init_name_server(void);

#endif /* NM_COMMON_H */
