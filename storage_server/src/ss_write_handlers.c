#include "ss_handlers.h"
#include "ss_file_ops.h"
#include "ss_locking.h"
#include "ss_session.h"
#include "ss_utils.h"

extern __thread ClientLogContext g_log_ctx;

void handle_write_begin(int client, const char *filename, int sentence_index, 
                        WriteSession *session, const char *username) {
    if (!session) {
        send_error(client, "UNKNOWN");
        return;
    }

    if (sentence_index < 0) {
        send_error(client, "INVALID_INDEX");
        return;
    }

    if (session->active) {
        send_error(client, "SENTENCE LOCKED");
        return;
    }

    char *content = load_file(filename);
    if (!content) {
        send_error(client, "FILE_NOT_FOUND");
        return;
    }

    char **sentences = NULL;
    int sentence_count = 0;
    if (!split_into_sentences(content, &sentences, &sentence_count)) {
        free(content);
        send_error(client, "INVALID_INDEX");
        return;
    }

    int requested_index = sentence_index;
    bool append_mode = false;

    if (sentence_count == 0 && sentence_index == 0) {
        append_mode = true;
        char **tmp = (char **)realloc(sentences, sizeof(char *) * 1);
        if (!tmp) {
            free_string_array(sentences, sentence_count);
            free(content);
            send_error(client, "UNKNOWN");
            return;
        }
        sentences = tmp;
        sentences[0] = strdup("");
        if (!sentences[0]) {
            free_string_array(sentences, sentence_count);
            free(content);
            send_error(client, "UNKNOWN");
            return;
        }
        sentence_count = 1;
    } else if (sentence_index == sentence_count) {
        if (sentence_count > 0) {
            const char *prev_sentence = sentences[sentence_count - 1];
            size_t len = strlen(prev_sentence);
            if (len == 0 || (prev_sentence[len - 1] != '.' && prev_sentence[len - 1] != '!' && 
                prev_sentence[len - 1] != '?')) {
                free_string_array(sentences, sentence_count);
                free(content);
                send_error(client, "INVALID_INDEX");
                return;
            }
        }
        
        append_mode = true;
        char **tmp = (char **)realloc(sentences, sizeof(char *) * (sentence_count + 1));
        if (!tmp) {
            free_string_array(sentences, sentence_count);
            free(content);
            send_error(client, "UNKNOWN");
            return;
        }
        sentences = tmp;
        sentences[sentence_count] = strdup("");
        if (!sentences[sentence_count]) {
            free_string_array(sentences, sentence_count);
            free(content);
            send_error(client, "UNKNOWN");
            return;
        }
        sentence_count++;
    } else if (sentence_index > sentence_count) {
        free_string_array(sentences, sentence_count);
        free(content);
        send_error(client, "INVALID_INDEX");
        return;
    }

    int slot = acquire_sentence_lock(filename, sentence_index, client);
    if (slot < 0) {
        free_string_array(sentences, sentence_count);
        free(content);
        send_error(client, "SENTENCE LOCKED");
        return;
    }

    char **words = NULL;
    int word_count = 0;
    int word_capacity = 0;
    char punctuation = 0;
    if (!split_sentence_into_words(sentences[sentence_index], &words, &word_count, 
                                    &word_capacity, &punctuation)) {
        release_sentence_lock_slot(slot);
        free_string_array(sentences, sentence_count);
        free(content);
        send_error(client, "UNKNOWN");
        return;
    }

    session_reset(session);
    session->active = true;
    session->owner_fd = client;
    session->lock_slot = slot;
    strncpy(session->filename, filename, sizeof(session->filename) - 1);
    session->filename[sizeof(session->filename) - 1] = '\0';
    session->sentence_index = sentence_index;
    session->original_sentence_index = requested_index;
    session->append_mode = append_mode;
    session->sentences = sentences;
    session->sentence_count = sentence_count;
    session->words = words;
    session->word_count = word_count;
    session->word_capacity = word_capacity;
    session->trailing_punct = punctuation;
    session->original_content = content;
    session->dirty = false;
    if (username) {
        strncpy(session->username, username, sizeof(session->username) - 1);
        session->username[sizeof(session->username) - 1] = '\0';
    } else {
        session->username[0] = '\0';
    }

    send_ok_message(client, "LOCKED");
}

void handle_update(int client, int word_index, const char *content, 
                   WriteSession *session, int replace_word) {
    if (!session || !session->active) {
        send_error(client, "NO_ACTIVE_WRITE");
        return;
    }

    if (word_index < 0) {
        send_error(client, "INVALID_INDEX");
        return;
    }

    if (!content) content = "";
    
    char **new_words = NULL;
    int new_word_count = 0;
    int new_word_capacity = 0;
    char content_punct = 0;
    
    if (!split_sentence_into_words(content, &new_words, &new_word_count, 
                                    &new_word_capacity, &content_punct)) {
        send_error(client, "UNKNOWN");
        return;
    }
    
    // Reattach punctuation to words that won't end up at the sentence end
    // Only the final word in the sentence should have its punctuation stripped
    if (content_punct != 0 && new_word_count > 0) {
        // Put punctuation back on the last new word
        // It will be stripped later by refresh_trailing_punctuation if it becomes the last word
        char *last_word = new_words[new_word_count - 1];
        size_t len = strlen(last_word);
        char *with_punct = malloc(len + 2);
        if (with_punct) {
            strcpy(with_punct, last_word);
            with_punct[len] = content_punct;
            with_punct[len + 1] = '\0';
            free(new_words[new_word_count - 1]);
            new_words[new_word_count - 1] = with_punct;
        }
    }
    
    if (new_word_count == 0 && !replace_word) {
        free_string_array(new_words, new_word_count);
        send_error(client, "BAD_REQUEST");
        return;
    }

    if (replace_word) {
        if (word_index >= session->word_count) {
            free_string_array(new_words, new_word_count);
            send_error(client, "INVALID_INDEX");
            return;
        }
        
        int new_total = session->word_count - 1 + new_word_count;
        int required_capacity = new_total;
        
        if (required_capacity > session->word_capacity) {
            int new_capacity = session->word_capacity > 0 ? session->word_capacity : 4;
            while (new_capacity < required_capacity) {
                new_capacity *= 2;
            }
            char **tmp = realloc(session->words, sizeof(char *) * new_capacity);
            if (!tmp) {
                free_string_array(new_words, new_word_count);
                send_error(client, "UNKNOWN");
                return;
            }
            session->words = tmp;
            session->word_capacity = new_capacity;
        }
        
        free(session->words[word_index]);
        
        if (new_word_count == 1) {
            session->words[word_index] = new_words[0];
            free(new_words);
        } else if (new_word_count == 0) {
            memmove(&session->words[word_index],
                    &session->words[word_index + 1],
                    sizeof(char *) * (session->word_count - word_index - 1));
            session->word_count--;
            free(new_words);
        } else {
            memmove(&session->words[word_index + new_word_count],
                    &session->words[word_index + 1],
                    sizeof(char *) * (session->word_count - word_index - 1));
            
            for (int i = 0; i < new_word_count; i++) {
                session->words[word_index + i] = new_words[i];
            }
            session->word_count = new_total;
            free(new_words);
        }
    } else {
        if (word_index > session->word_count) {
            free_string_array(new_words, new_word_count);
            send_error(client, "INVALID_INDEX");
            return;
        }
        
        int new_total = session->word_count + new_word_count;
        int required_capacity = new_total;
        
        if (required_capacity > session->word_capacity) {
            int new_capacity = session->word_capacity > 0 ? session->word_capacity : 4;
            while (new_capacity < required_capacity) {
                new_capacity *= 2;
            }
            char **tmp = realloc(session->words, sizeof(char *) * new_capacity);
            if (!tmp) {
                free_string_array(new_words, new_word_count);
                send_error(client, "UNKNOWN");
                return;
            }
            session->words = tmp;
            session->word_capacity = new_capacity;
        }

        if (word_index < session->word_count) {
            memmove(&session->words[word_index + new_word_count],
                    &session->words[word_index],
                    sizeof(char *) * (session->word_count - word_index));
        }
        
        for (int i = 0; i < new_word_count; i++) {
            session->words[word_index + i] = new_words[i];
        }
        session->word_count = new_total;
        free(new_words);
    }

    refresh_trailing_punctuation(session);
    session->dirty = true;
    send_ok_message(client, "UPDATED");
}

// Commit runs under the file's lock: reload, find the sentence this session
// started from, splice in the new text, snapshot the version being replaced
// (for UNDO), and save. Returns 1 if the file changed.
int handle_commit(int client, WriteSession *session) {
    if (!session || !session->active) {
        send_error(client, "NO_ACTIVE_WRITE");
        return 0;
    }

    // Nothing was edited: leave the file and its undo snapshot alone.
    if (!session->dirty) {
        send_ok_message(client, "WRITE DONE");
        session_reset(session);
        return 0;
    }

    refresh_trailing_punctuation(session);

    char *updated_text = join_words(session->words, session->word_count, session->trailing_punct);
    if (!updated_text) {
        send_error(client, "UNKNOWN");
        session_reset(session);
        return 0;
    }

    char **replacement = NULL;
    int replacement_count = 0;
    int replacement_capacity = 0;

    if (!split_into_sentences(updated_text, &replacement, &replacement_count) || 
        replacement_count == 0) {
        free_string_array(replacement, replacement_count);
        replacement = NULL;
        replacement_count = 0;
        replacement_capacity = 0;
        if (!append_sentence(&replacement, &replacement_count, &replacement_capacity, updated_text)) {
            free(updated_text);
            send_error(client, "UNKNOWN");
            session_reset(session);
            return 0;
        }
    }
    free(updated_text);

    const char *error = NULL;
    char *latest_content = NULL;
    char **current_sentences = NULL;
    int current_count = 0;
    char **merged = NULL;
    int merged_count = 0;
    char *new_content = NULL;

    file_lock(session->filename);

    latest_content = load_file(session->filename);
    if (!latest_content) {
        error = "FILE_NOT_FOUND";
        goto done;
    }
    if (!split_into_sentences(latest_content, &current_sentences, &current_count)) {
        error = "UNKNOWN";
        goto done;
    }

    int target_index = -1;
    if (session->append_mode) {
        target_index = current_count;
    } else {
        const char *baseline = NULL;
        if (session->sentences && session->original_sentence_index >= 0 &&
            session->original_sentence_index < session->sentence_count) {
            baseline = session->sentences[session->original_sentence_index];
        }
        if (baseline && *baseline) {
            target_index = find_sentence_index_with_hint(current_sentences, current_count, 
                                                         baseline, session->original_sentence_index);
        }
        // The sentence was changed by someone else since this WRITE began.
        // Guessing a position could overwrite a different sentence, so refuse.
        if (target_index < 0) {
            error = "CONFLICT";
            goto done;
        }
    }

    int merged_capacity = current_count + replacement_count + 4;
    merged = (char **)malloc(sizeof(char *) * merged_capacity);
    if (!merged) {
        error = "UNKNOWN";
        goto done;
    }

    int copy_limit = (target_index < current_count) ? target_index : current_count;
    for (int i = 0; i < copy_limit && !error; i++) {
        if (!append_sentence(&merged, &merged_count, &merged_capacity, current_sentences[i])) {
            error = "UNKNOWN";
        }
    }
    for (int i = 0; i < replacement_count && !error; i++) {
        if (!append_sentence(&merged, &merged_count, &merged_capacity, replacement[i])) {
            error = "UNKNOWN";
        }
    }
    int start_index = current_count;
    if (!session->append_mode && target_index < current_count) {
        start_index = target_index + 1;
    }
    for (int i = start_index; i < current_count && !error; i++) {
        if (!append_sentence(&merged, &merged_count, &merged_capacity, current_sentences[i])) {
            error = "UNKNOWN";
        }
    }
    if (error) {
        goto done;
    }

    new_content = join_sentences(merged, merged_count);
    if (!new_content) {
        error = "UNKNOWN";
        goto done;
    }

    // The snapshot is the version this commit replaces, so UNDO never
    // reverts someone else's commit.
    if (save_snapshot(session->filename, latest_content) != 0 ||
        save_file_atomic(session->filename, new_content) != 0) {
        error = "UNKNOWN";
    }

done:
    file_unlock(session->filename);
    free(new_content);
    free(latest_content);
    free_string_array(current_sentences, current_count);
    free_string_array(replacement, replacement_count);
    free_string_array(merged, merged_count);

    if (error) {
        send_error(client, error);
        session_reset(session);
        return 0;
    }
    send_ok_message(client, "WRITE DONE");
    session_reset(session);
    return 1;
}
