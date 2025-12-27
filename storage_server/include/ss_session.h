#ifndef SS_SESSION_H
#define SS_SESSION_H

#include "ss_common.h"

// Write session structure
typedef struct {
    bool active;
    int owner_fd;
    int lock_slot;
    char filename[MAX_FILENAME];
    int sentence_index;
    int original_sentence_index;
    bool append_mode;
    char username[MAX_USERNAME];
    char **sentences;
    int sentence_count;
    char **words;
    int word_count;
    int word_capacity;
    char trailing_punct;
    char *original_content;
    bool dirty;
} WriteSession;

// Session management
void session_init(WriteSession *session, int owner_fd);
void session_reset(WriteSession *session);

// String array utilities
void free_string_array(char **arr, int count);
int split_into_sentences(const char *content, char ***out_sentences, int *out_count);
int split_sentence_into_words(const char *sentence, char ***out_words, int *out_count, 
                               int *out_capacity, char *punctuation);
char *join_words(char **words, int word_count, char punctuation);
char *join_sentences(char **sentences, int count);
void refresh_trailing_punctuation(WriteSession *session);
int find_sentence_index_with_hint(char **sentences, int count, const char *target, int hint_index);

#endif // SS_SESSION_H
