#ifndef SS_LOCKING_H
#define SS_LOCKING_H

#include "ss_common.h"

// Sentence lock structure
typedef struct {
    bool active;
    char filename[MAX_FILENAME];
    int sentence_index;
    int owner_fd;
} SentenceLock;

// Lock management functions
void locking_init(void);
int acquire_sentence_lock(const char *filename, int sentence_index, int owner_fd);
void release_sentence_lock_slot(int slot);
void release_sentence_locks_for_owner(int owner_fd);
bool file_has_active_lock(const char *filename);

#endif // SS_LOCKING_H
