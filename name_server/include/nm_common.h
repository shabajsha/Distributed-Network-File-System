#ifndef NM_COMMON_H
#define NM_COMMON_H

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

#include "dnfs_shared.h"

#define MAX_USERS 1024
#define MAX_SESSIONS 1024
#define MAX_STORAGE_SERVERS 32
#define HASH_BUCKETS 1024
#define MAX_FILENAME 256
#define MAX_USERNAME 64
#define PASS_HASH_LEN 160
#define SECRET_LEN 128
#define CACHE_SIZE 50
#define MAX_CONNECTION_THREADS 256

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
    char pass_hash[PASS_HASH_LEN];
    char ip[INET_ADDRSTRLEN];
    time_t last_login;
} User;

typedef struct {
    char token[DNFS_HEX_LEN + 1];
    char username[MAX_USERNAME];
    time_t last_used;
} Session;

typedef struct {
    char ip[INET_ADDRSTRLEN];
    int client_port;
    time_t registered_at;
    time_t last_seen;
} StorageServer;

typedef struct AccessEntry {
    char username[MAX_USERNAME];
    char mode[3];
    struct AccessEntry *next;
} AccessEntry;

/* A replica is "stale" when it missed a commit (its server was down or
 * lost the file); it is refreshed from the other replica when possible. */
struct FileMetadata {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    char backup_ss_ip[INET_ADDRSTRLEN];
    int backup_ss_port;
    int primary_stale;
    int backup_stale;
    int pending;
    AccessEntry *access_list;
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

extern char BASE_DIR[256];
extern char LOG_DIR[1024];
extern char METADATA_FILE[1024];
extern char NM_SECRET[SECRET_LEN];
extern int ss_timeout_sec;

extern User users[MAX_USERS];
extern int user_count;
extern Session sessions[MAX_SESSIONS];
extern StorageServer storage_servers[MAX_STORAGE_SERVERS];
extern int ss_count;
extern HashNode *file_hash_table[HASH_BUCKETS];
extern LRUCache file_cache;

/* Lock order: save_mutex -> files_mutex -> users_mutex. ss_mutex and
 * replication_mutex are never held together with another of these. */
extern pthread_mutex_t users_mutex;
extern pthread_mutex_t ss_mutex;
extern pthread_mutex_t files_mutex;
extern pthread_mutex_t save_mutex;
extern pthread_mutex_t replication_mutex;
extern FILE *log_file;
extern pthread_mutex_t log_mutex;

void init_name_server(void);

#endif /* NM_COMMON_H */
