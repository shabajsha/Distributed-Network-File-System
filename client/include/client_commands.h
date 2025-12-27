#ifndef CLIENT_COMMANDS_H
#define CLIENT_COMMANDS_H

#include "client_common.h"

void handle_view(const char *flags);
void handle_list(void);
void handle_create(const char *filename);
void handle_info(const char *filename);
void handle_addaccess(const char *filename, const char *target, const char *mode);
void handle_remaccess(const char *filename, const char *target);
void handle_read(const char *filename);
void handle_write(const char *filename, int sentence_index);
void handle_stream(const char *filename);
void handle_undo(const char *filename);
void handle_delete(const char *filename);
void handle_exec(const char *filename);
void print_help(void);

#endif /* CLIENT_COMMANDS_H */
