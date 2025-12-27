#include "nm_cache.h"
#include "nm_logging.h"

static void move_to_front(CacheNode *node) {
    if (!node || node == file_cache.head) {
        return;
    }

    if (node->prev) {
        node->prev->next = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    if (node == file_cache.tail) {
        file_cache.tail = node->prev;
    }

    node->prev = NULL;
    node->next = file_cache.head;
    if (file_cache.head) {
        file_cache.head->prev = node;
    }
    file_cache.head = node;
    if (!file_cache.tail) {
        file_cache.tail = node;
    }
}

void init_cache(void) {
    file_cache.head = NULL;
    file_cache.tail = NULL;
    file_cache.size = 0;
    pthread_mutex_init(&file_cache.mutex, NULL);
}

FileMetadata *cache_get(const char *filename) {
    if (!filename) {
        return NULL;
    }

    pthread_mutex_lock(&file_cache.mutex);

    CacheNode *current = file_cache.head;
    while (current) {
        if (strcmp(current->filename, filename) == 0) {
            current->last_access = time(NULL);
            move_to_front(current);
            FileMetadata *result = current->file;
            pthread_mutex_unlock(&file_cache.mutex);
            return result;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&file_cache.mutex);
    return NULL;
}

void cache_put(const char *filename, FileMetadata *file) {
    if (!filename || !file) {
        return;
    }

    pthread_mutex_lock(&file_cache.mutex);

    CacheNode *current = file_cache.head;
    while (current) {
        if (strcmp(current->filename, filename) == 0) {
            current->file = file;
            current->last_access = time(NULL);
            move_to_front(current);
            pthread_mutex_unlock(&file_cache.mutex);
            return;
        }
        current = current->next;
    }

    CacheNode *new_node = malloc(sizeof(CacheNode));
    if (!new_node) {
        pthread_mutex_unlock(&file_cache.mutex);
        return;
    }

    strncpy(new_node->filename, filename, sizeof(new_node->filename) - 1);
    new_node->filename[sizeof(new_node->filename) - 1] = '\0';
    new_node->file = file;
    new_node->last_access = time(NULL);
    new_node->prev = NULL;
    new_node->next = file_cache.head;

    if (file_cache.head) {
        file_cache.head->prev = new_node;
    }
    file_cache.head = new_node;
    if (!file_cache.tail) {
        file_cache.tail = new_node;
    }

    file_cache.size++;

    if (file_cache.size > CACHE_SIZE) {
        CacheNode *lru = file_cache.tail;
        if (lru) {
            if (lru->prev) {
                lru->prev->next = NULL;
            }
            file_cache.tail = lru->prev;

            char log_msg[512];
            snprintf(log_msg, sizeof(log_msg), "Cache evicted (LRU): %s", lru->filename);
            log_message("INFO", log_msg, "0.0.0.0", 0, "cache");
            free(lru);
            file_cache.size--;
        }
    }

    pthread_mutex_unlock(&file_cache.mutex);
}

void cache_remove(const char *filename) {
    if (!filename) {
        return;
    }

    pthread_mutex_lock(&file_cache.mutex);

    CacheNode *current = file_cache.head;
    while (current) {
        if (strcmp(current->filename, filename) == 0) {
            if (current->prev) {
                current->prev->next = current->next;
            }
            if (current->next) {
                current->next->prev = current->prev;
            }
            if (current == file_cache.head) {
                file_cache.head = current->next;
            }
            if (current == file_cache.tail) {
                file_cache.tail = current->prev;
            }
            free(current);
            file_cache.size--;
            break;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&file_cache.mutex);
}
