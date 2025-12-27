#ifndef NM_HANDLERS_H
#define NM_HANDLERS_H

#include "nm_common.h"

void handle_register_client(int client_fd, const char *request, const char *client_ip);
void handle_register_ss(int ss_fd, const char *request, const char *ss_ip);
void handle_view(int client_fd, const char *request, const char *username);
void handle_list(int client_fd, const char *username);
void handle_create(int client_fd, const char *request, const char *username);
void handle_info(int client_fd, const char *request, const char *username);
void handle_addaccess(int client_fd, const char *request, const char *username);
void handle_remaccess(int client_fd, const char *request, const char *username);
void handle_file_operation(int client_fd, const char *request, const char *username);
void handle_delete(int client_fd, const char *request, const char *username);
void handle_exec(int client_fd, const char *request, const char *username);

#endif /* NM_HANDLERS_H */
