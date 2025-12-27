#ifndef SS_HANDLERS_H
#define SS_HANDLERS_H

#include "ss_common.h"
#include "ss_session.h"

// Command handlers
void handle_create_file(int client, const char *filename, const char *initial_content);
void handle_read(int client, const char *filename);
void handle_write_begin(int client, const char *filename, int sentence_index, 
                        WriteSession *session, const char *username);
void handle_update(int client, int word_index, const char *content, 
                   WriteSession *session, int replace_word);
void handle_commit(int client, WriteSession *session);
void handle_undo(int client, const char *filename);
void handle_stream(int client, const char *filename);
void handle_stat(int client, const char *filename);

// Message sending utilities
void send_json(int client, const char* json);
void send_error(int client, const char *reason);
void send_ok_message(int client, const char *msg);
void send_plain_line(int client, const char *line, size_t len);

#endif // SS_HANDLERS_H
