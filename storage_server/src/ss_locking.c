#include "ss_locking.h"

static SentenceLock g_sentence_locks[MAX_LOCKS];
static pthread_mutex_t g_lock_mutex = PTHREAD_MUTEX_INITIALIZER;

void locking_init(void) {
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
