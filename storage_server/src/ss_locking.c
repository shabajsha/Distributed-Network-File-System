#include "ss_locking.h"

#define FILE_LOCK_STRIPES 64

static SentenceLock g_sentence_locks[MAX_LOCKS];
static pthread_mutex_t g_lock_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_file_mutexes[FILE_LOCK_STRIPES];

void locking_init(void) {
    for (int i = 0; i < FILE_LOCK_STRIPES; i++) {
        pthread_mutex_init(&g_file_mutexes[i], NULL);
    }

    pthread_mutex_lock(&g_lock_mutex);
    for (int i = 0; i < MAX_LOCKS; i++) {
        g_sentence_locks[i].active = false;
        g_sentence_locks[i].filename[0] = '\0';
        g_sentence_locks[i].sentence_index = 0;
        g_sentence_locks[i].owner_fd = -1;
    }
    pthread_mutex_unlock(&g_lock_mutex);
}

int acquire_sentence_lock(const char *filename, int sentence_index, int owner_fd) {
    int result = -1;

    pthread_mutex_lock(&g_lock_mutex);

    for (int i = 0; i < MAX_LOCKS; i++) {
        SentenceLock *lock = &g_sentence_locks[i];
        if (lock->active && lock->sentence_index == sentence_index && 
            strcmp(lock->filename, filename) == 0) {
            if (lock->owner_fd == owner_fd) {
                result = i;
            }
            pthread_mutex_unlock(&g_lock_mutex);
            return result;
        }
    }

    for (int i = 0; i < MAX_LOCKS; i++) {
        SentenceLock *lock = &g_sentence_locks[i];
        if (!lock->active) {
            lock->active = true;
            lock->sentence_index = sentence_index;
            lock->owner_fd = owner_fd;
            strncpy(lock->filename, filename, sizeof(lock->filename) - 1);
            lock->filename[sizeof(lock->filename) - 1] = '\0';
            result = i;
            break;
        }
    }

    pthread_mutex_unlock(&g_lock_mutex);
    return result;
}

void release_sentence_lock_slot(int slot) {
    if (slot < 0 || slot >= MAX_LOCKS) return;

    pthread_mutex_lock(&g_lock_mutex);
    g_sentence_locks[slot].active = false;
    g_sentence_locks[slot].filename[0] = '\0';
    g_sentence_locks[slot].sentence_index = 0;
    g_sentence_locks[slot].owner_fd = -1;
    pthread_mutex_unlock(&g_lock_mutex);
}

void release_sentence_locks_for_owner(int owner_fd) {
    pthread_mutex_lock(&g_lock_mutex);
    for (int i = 0; i < MAX_LOCKS; i++) {
        if (g_sentence_locks[i].active && g_sentence_locks[i].owner_fd == owner_fd) {
            g_sentence_locks[i].active = false;
            g_sentence_locks[i].filename[0] = '\0';
            g_sentence_locks[i].sentence_index = 0;
            g_sentence_locks[i].owner_fd = -1;
        }
    }
    pthread_mutex_unlock(&g_lock_mutex);
}

bool file_has_active_lock(const char *filename) {
    bool locked = false;

    pthread_mutex_lock(&g_lock_mutex);
    for (int i = 0; i < MAX_LOCKS; i++) {
        if (g_sentence_locks[i].active && strcmp(g_sentence_locks[i].filename, filename) == 0) {
            locked = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock_mutex);
    return locked;
}

// Files hash onto a fixed set of mutexes, so two files may share one; that
// only costs a little waiting, as long as nobody holds two at once.
static pthread_mutex_t *file_mutex_for(const char *filename) {
    unsigned int hash = 5381;
    for (const char *p = filename; *p; p++) {
        hash = ((hash << 5) + hash) + (unsigned char)*p;
    }
    return &g_file_mutexes[hash % FILE_LOCK_STRIPES];
}

void file_lock(const char *filename) {
    pthread_mutex_lock(file_mutex_for(filename));
}

void file_unlock(const char *filename) {
    pthread_mutex_unlock(file_mutex_for(filename));
}
