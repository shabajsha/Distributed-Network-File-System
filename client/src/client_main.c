//AI code starts
#include "client_common.h"
#include "client_commands.h"
#include "client_network.h"

char NM_IP[INET_ADDRSTRLEN] = "127.0.0.1";
char current_username[MAX_USERNAME];

int main(int argc, char *argv[]) {
    if (argc > 1) {
        strncpy(NM_IP, argv[1], INET_ADDRSTRLEN - 1);
        NM_IP[INET_ADDRSTRLEN - 1] = '\0';
        printf("Connecting to Name Server at %s:%d\n", NM_IP, NM_PORT);
    } else {
        printf("Usage: %s [name_server_ip]\n", argv[0]);
        printf("Using default Name Server IP: %s\n", NM_IP);
    }

    printf("Enter username: ");
    if (!fgets(current_username, sizeof(current_username), stdin)) {
        fprintf(stderr, "Failed to read username\n");
        return 1;
    }
    current_username[strcspn(current_username, "\n")] = 0;
    if (strlen(current_username) == 0) {
        fprintf(stderr, "Username cannot be empty\n");
        return 1;
    }

    int nm_fd = connect_to_nm();
    if (nm_fd < 0) {
        fprintf(stderr, "Error: Could not connect to Name Server\n");
        return 1;
    }

    struct sockaddr_in local_addr = {0};
    socklen_t addr_len = sizeof(local_addr);
    char client_ip[INET_ADDRSTRLEN] = "127.0.0.1";
    if (getsockname(nm_fd, (struct sockaddr *)&local_addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &local_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    }

    char register_msg[512];
    snprintf(register_msg, sizeof(register_msg),
             "{\"cmd\":\"register_client\",\"username\":\"%s\",\"ip\":\"%s\",\"nm_port\":%d,\"ss_port\":%d}",
             current_username, client_ip, NM_PORT, NM_PORT + 100);

    send_message(nm_fd, register_msg);
    char *response = receive_message(nm_fd);

    if (response && strstr(response, "\"status\":\"OK\"")) {
        printf("Successfully registered as '%s'\n\n", current_username);
    } else {
        fprintf(stderr, "Registration failed\n");
        if (response) free(response);
        close(nm_fd);
        return 1;
    }

    if (response) free(response);
    close(nm_fd);

    printf("Type 'help' for available commands\n\n");

    char input[1024];
    while (1) {
        printf("%s> ", current_username);
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }

        input[strcspn(input, "\n")] = 0;
        if (strlen(input) == 0) {
            continue;
        }

        char cmd[64] = {0};
        sscanf(input, "%s", cmd);

        if (strcmp(cmd, "help") == 0) {
            print_help();
        } else if (strcmp(cmd, "VIEW") == 0) {
            char flags[16] = {0};
            sscanf(input, "VIEW %s", flags);
            handle_view(flags);
        } else if (strcmp(cmd, "LIST") == 0) {
            handle_list();
        } else if (strcmp(cmd, "CREATE") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "CREATE %s", filename) == 1) {
                handle_create(filename);
            } else {
                printf("Usage: CREATE <filename>\n");
            }
        } else if (strcmp(cmd, "INFO") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "INFO %s", filename) == 1) {
                handle_info(filename);
            } else {
                printf("Usage: INFO <filename>\n");
            }
        } else if (strcmp(cmd, "ADDACCESS") == 0) {
            char mode_flag[4];
            char filename[MAX_FILENAME];
            char target[MAX_USERNAME];
            if (sscanf(input, "ADDACCESS %s %s %s", mode_flag, filename, target) == 3) {
                char mode[3] = {0};
                if (strcmp(mode_flag, "-R") == 0) {
                    strcpy(mode, "R");
                } else if (strcmp(mode_flag, "-W") == 0) {
                    strcpy(mode, "W");
                } else {
                    printf("Invalid mode. Use -R or -W\n");
                    continue;
                }
                handle_addaccess(filename, target, mode);
            } else {
                printf("Usage: ADDACCESS -R/-W <filename> <username>\n");
            }
        } else if (strcmp(cmd, "REMACCESS") == 0) {
            char filename[MAX_FILENAME];
            char target[MAX_USERNAME];
            if (sscanf(input, "REMACCESS %s %s", filename, target) == 2) {
                handle_remaccess(filename, target);
            } else {
                printf("Usage: REMACCESS <filename> <username>\n");
            }
        } else if (strcmp(cmd, "READ") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "READ %s", filename) == 1) {
                handle_read(filename);
            } else {
                printf("Usage: READ <filename>\n");
            }
        } else if (strcmp(cmd, "WRITE") == 0) {
            char filename[MAX_FILENAME];
            int sentence_index = 0;
            if (sscanf(input, "WRITE %s %d", filename, &sentence_index) == 2) {
                handle_write(filename, sentence_index);
            } else {
                printf("Usage: WRITE <filename> <sentence_number>\n");
            }
        } else if (strcmp(cmd, "STREAM") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "STREAM %s", filename) == 1) {
                handle_stream(filename);
            } else {
                printf("Usage: STREAM <filename>\n");
            }
        } else if (strcmp(cmd, "UNDO") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "UNDO %s", filename) == 1) {
                handle_undo(filename);
            } else {
                printf("Usage: UNDO <filename>\n");
            }
        } else if (strcmp(cmd, "DELETE") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "DELETE %s", filename) == 1) {
                handle_delete(filename);
            } else {
                printf("Usage: DELETE <filename>\n");
            }
        } else if (strcmp(cmd, "EXEC") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "EXEC %s", filename) == 1) {
                handle_exec(filename);
            } else {
                printf("Usage: EXEC <filename>\n");
            }
        } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            printf("Goodbye!\n");
            break;
        } else {
            printf("Unknown command. Type 'help' for available commands.\n");
        }
    }

    return 0;
}
//AI code ends