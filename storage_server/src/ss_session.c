#include "ss_session.h"
#include "ss_locking.h"

void session_init(WriteSession *session, int owner_fd) {
    if (!session) return;
    memset(session, 0, sizeof(*session));
    session->lock_slot = -1;
    session->owner_fd = owner_fd;
    session->original_sentence_index = -1;
}

void session_reset(WriteSession *session) {
    if (!session) return;
    if (session->lock_slot >= 0) {
        release_sentence_lock_slot(session->lock_slot);
    }
    free_string_array(session->words, session->word_count);
    free_string_array(session->sentences, session->sentence_count);
    free(session->original_content);
    memset(session, 0, sizeof(*session));
    session->lock_slot = -1;
    session->owner_fd = -1;
    session->original_sentence_index = -1;
}

void free_string_array(char **arr, int count) {
    if (!arr) return;
    for (int i = 0; i < count; i++) {
        free(arr[i]);
    }
    free(arr);
}

static int append_sentence(char ***arr, int *count, int *capacity, const char *src) {
    if (!arr || !count || !capacity) return 0;
    if (*count >= *capacity) {
        int new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        char **tmp = (char **)realloc(*arr, sizeof(char *) * new_capacity);
        if (!tmp) {
            return 0;
        }
        *arr = tmp;
        *capacity = new_capacity;
    }

    (*arr)[*count] = strdup(src ? src : "");
    if (!(*arr)[*count]) {
        return 0;
    }
    (*count)++;
    return 1;
}

int split_into_sentences(const char *content, char ***out_sentences, int *out_count) {
    if (!content || !out_sentences || !out_count) return 0;

    int capacity = 8;
    int count = 0;
    char **sentences = malloc(sizeof(char *) * capacity);
    if (!sentences) return 0;

    const char *p = content;
    const char *start = content;
    while (*p) {
        if (*p == '.' || *p == '!' || *p == '?') {
            size_t len = (size_t)(p - start + 1);
            char *sentence = malloc(len + 1);
            if (!sentence) {
                free_string_array(sentences, count);
                return 0;
            }
            memcpy(sentence, start, len);
            sentence[len] = '\0';

            if (count == capacity) {
                capacity *= 2;
                char **tmp = realloc(sentences, sizeof(char *) * capacity);
                if (!tmp) {
                    free(sentence);
                    free_string_array(sentences, count);
                    return 0;
                }
                sentences = tmp;
            }
            sentences[count++] = sentence;
            p++;
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            start = p;
            continue;
        }
        p++;
    }

    if (start && *start) {
        size_t len = (size_t)(p - start);
        char *sentence = malloc(len + 1);
        if (!sentence) {
            free_string_array(sentences, count);
            return 0;
        }
        memcpy(sentence, start, len);
        sentence[len] = '\0';
        if (count == capacity) {
            capacity *= 2;
            char **tmp = realloc(sentences, sizeof(char *) * capacity);
            if (!tmp) {
                free(sentence);
                free_string_array(sentences, count);
                return 0;
            }
            sentences = tmp;
        }
        sentences[count++] = sentence;
    }

    *out_sentences = sentences;
    *out_count = count;
    return 1;
}

static const char *skip_spaces(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

int split_sentence_into_words(const char *sentence, char ***out_words, int *out_count, 
                               int *out_capacity, char *punctuation) {
    if (!sentence || !out_words || !out_count || !out_capacity || !punctuation) return 0;

    size_t len = strlen(sentence);
    char punct = 0;
    if (len > 0) {
        char last = sentence[len - 1];
        if (last == '.' || last == '!' || last == '?') {
            punct = last;
        }
    }

    char *copy = strdup(sentence);
    if (!copy) return 0;

    int capacity = 8;
    int count = 0;
    char **words = malloc(sizeof(char *) * capacity);
    if (!words) {
        free(copy);
        return 0;
    }

    char *token = copy;
    while (*token) {
        token = (char *)skip_spaces(token);
        if (!*token) break;

        char *end = token;
        while (*end && !isspace((unsigned char)*end)) {
            end++;
        }

        size_t wlen = (size_t)(end - token);
        char *word = malloc(wlen + 1);
        if (!word) {
            free_string_array(words, count);
            free(copy);
            return 0;
        }
        memcpy(word, token, wlen);
        word[wlen] = '\0';

        if (count == capacity) {
            capacity *= 2;
            char **tmp = realloc(words, sizeof(char *) * capacity);
            if (!tmp) {
                free(word);
                free_string_array(words, count);
                free(copy);
                return 0;
            }
            words = tmp;
        }

        words[count++] = word;
        token = end;
    }

    // Remove trailing punctuation from the last word if it matches the sentence punctuation
    if (count > 0 && punct != 0) {
        char *last_word = words[count - 1];
        size_t last_len = strlen(last_word);
        if (last_len > 0 && last_word[last_len - 1] == punct) {
            last_word[last_len - 1] = '\0';
        }
    }

    free(copy);
    *out_words = words;
    *out_count = count;
    *out_capacity = capacity;
    *punctuation = punct;
    return 1;
}

char *join_words(char **words, int word_count, char punctuation) {
    size_t total = 0;
    for (int i = 0; i < word_count; i++) {
        total += strlen(words[i]);
        if (i != word_count - 1) total += 1;
    }

    int append_punct = 0;
    if (punctuation) {
        if (word_count == 0) {
            append_punct = 1;
        } else {
            char *last = words[word_count - 1];
            size_t len = strlen(last);
            if (len == 0 || last[len - 1] != punctuation) {
                append_punct = 1;
            }
        }
    }

    if (append_punct) {
        total += 1;
    }

    char *sentence = malloc(total + 1);
    if (!sentence) return NULL;

    sentence[0] = '\0';
    for (int i = 0; i < word_count; i++) {
        strcat(sentence, words[i]);
        if (i != word_count - 1) strcat(sentence, " ");
    }

    if (append_punct) {
        size_t len = strlen(sentence);
        sentence[len] = punctuation;
        sentence[len + 1] = '\0';
    }

    return sentence;
}

char *join_sentences(char **sentences, int count) {
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        total += strlen(sentences[i]);
        if (i != count - 1) total += 1;
    }
    char *result = malloc(total + 1);
    if (!result) return NULL;
    result[0] = '\0';
    for (int i = 0; i < count; i++) {
        strcat(result, sentences[i]);
        if (i != count - 1) strcat(result, " ");
    }
    return result;
}

void refresh_trailing_punctuation(WriteSession *session) {
    if (!session) return;

    // Check if the last word has punctuation attached
    while (session->word_count > 0) {
        char *last = session->words[session->word_count - 1];
        if (!last) {
            session->word_count--;
            continue;
        }

        size_t len = strlen(last);
        if (len == 0) {
            free(last);
            session->words[session->word_count - 1] = NULL;
            session->word_count--;
            continue;
        }

        char ch = last[len - 1];
        if (ch == '.' || ch == '!' || ch == '?') {
            session->trailing_punct = ch;
        }
        break;
    }
}

int find_sentence_index_with_hint(char **sentences, int count, const char *target, int hint_index) {
    if (!sentences || !target || *target == '\0') {
        return -1;
    }

    if (hint_index >= 0 && hint_index < count && strcmp(sentences[hint_index], target) == 0) {
        return hint_index;
    }

    if (hint_index < 0 || hint_index >= count) {
        for (int i = 0; i < count; i++) {
            if (strcmp(sentences[i], target) == 0) {
                return i;
            }
        }
        return -1;
    }

    for (int offset = 1; offset < count; offset++) {
        int forward = hint_index + offset;
        if (forward < count && strcmp(sentences[forward], target) == 0) {
            return forward;
        }
        int backward = hint_index - offset;
        if (backward >= 0 && strcmp(sentences[backward], target) == 0) {
            return backward;
        }
    }
    return -1;
}
