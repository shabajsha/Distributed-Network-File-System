#ifndef SS_HANDLERS_H
#define SS_HANDLERS_H

#include "ss_common.h"
#include "ss_session.h"

// Command handlers. Those returning int return 1 when the file's content
// changed, so the caller can tell the name server to update the backup.
void handle_create_file(int client, const char *filename, const char *initial_content);
void handle_read(int client, const char *filename, int with_snapshot);
void handle_write_begin(int client, const char *filename, int sentence_index,
                        WriteSession *session, const char *username);
void handle_update(int client, int word_index, const char *content,
                   WriteSession *session, int replace_word);
int handle_commit(int client, WriteSession *session);
int handle_undo(int client, const char *filename);
void handle_stream(int client, const char *filename);
void handle_stat(int client, const char *filename);
void handle_put(int client, const char *filename, const char *content, const char *snapshot);
void handle_delete(int client, const char *filename);

// Message sending utilities
void send_json(int client, const char* json);
void send_error(int client, const char *reason);
void send_ok_message(int client, const char *msg);

#endif // SS_HANDLERS_H
