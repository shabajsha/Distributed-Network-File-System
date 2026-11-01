//AI code starts
#include "client_common.h"
#include "client_commands.h"
#include "client_network.h"

#include <termios.h>

char NM_IP[INET_ADDRSTRLEN] = "127.0.0.1";
char current_username[MAX_USERNAME];
char current_password[MAX_PASSWORD];
char session_token[DNFS_HEX_LEN + 1];

/* Reads a line without echoing it when stdin is a terminal. */
static int read_password(char *buf, size_t size) {
    struct termios old_attr, quiet_attr;
    int tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_attr) == 0;
    if (tty) {
        quiet_attr = old_attr;
        quiet_attr.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet_attr);
    }
    char *ok = fgets(buf, (int)size, stdin);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_attr);
        printf("\n");
    }
    if (!ok) {
        return 0;
    }
    buf[strcspn(buf, "\n")] = '\0';
    return buf[0] != '\0';
}

/* Accepts "VIEW", "VIEW -a", "VIEW -l", "VIEW -al" and "VIEW -a -l". */
static int parse_view_flags(const char *input, char *flags, size_t size) {
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", input);
    size_t len = 0;
    char *save = NULL;
    strtok_r(copy, " \t", &save);
    for (char *tok = strtok_r(NULL, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        if (tok[0] != '-' || !tok[1]) {
            return 0;
        }
        for (const char *f = tok + 1; *f; f++) {
            if ((*f != 'a' && *f != 'l') || len + 2 >= size) {
                return 0;
            }
            if (!strchr(flags, *f)) {
                if (len == 0) {
                    flags[len++] = '-';
                }
                flags[len++] = *f;
                flags[len] = '\0';
            }
        }
    }
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        snprintf(NM_IP, sizeof(NM_IP), "%s", argv[1]);
        printf("Connecting to Name Server at %s:%d\n", NM_IP, dnfs_nm_port());
    } else {
        printf("Usage: %s [name_server_ip]\n", argv[0]);
        printf("Using default Name Server IP: %s\n", NM_IP);
    }

    printf("Enter username: ");
    fflush(stdout);
    if (!fgets(current_username, sizeof(current_username), stdin)) {
        fprintf(stderr, "Failed to read username\n");
        return 1;
    }
    current_username[strcspn(current_username, "\n")] = 0;
    if (!dnfs_valid_username(current_username)) {
        fprintf(stderr, "Usernames are 1-63 letters, digits, '.', '_' or '-' and can't start with '.'\n");
        return 1;
    }

    printf("Password (a new username sets it): ");
    fflush(stdout);
    if (!read_password(current_password, sizeof(current_password))) {
        fprintf(stderr, "Password cannot be empty\n");
        return 1;
    }

    if (!login()) {
        return 1;
    }
    printf("Logged in as '%s'\n\n", current_username);
    printf("Type 'help' for available commands\n\n");

    char input[1024];
    while (1) {
        printf("%s> ", current_username);
        fflush(stdout);
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }

        input[strcspn(input, "\n")] = 0;
        if (strlen(input) == 0) {
            continue;
        }

        char cmd[64] = {0};
        sscanf(input, "%63s", cmd);

        if (strcmp(cmd, "help") == 0) {
            print_help();
        } else if (strcmp(cmd, "VIEW") == 0) {
            char flags[8] = {0};
            if (parse_view_flags(input, flags, sizeof(flags))) {
                handle_view(flags);
            } else {
                printf("Usage: VIEW [-a] [-l] [-al]\n");
            }
        } else if (strcmp(cmd, "LIST") == 0) {
            handle_list();
        } else if (strcmp(cmd, "CREATE") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "CREATE %255s", filename) == 1) {
                handle_create(filename);
            } else {
                printf("Usage: CREATE <filename>\n");
            }
        } else if (strcmp(cmd, "INFO") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "INFO %255s", filename) == 1) {
                handle_info(filename);
            } else {
                printf("Usage: INFO <filename>\n");
            }
        } else if (strcmp(cmd, "ADDACCESS") == 0) {
            char mode_flag[4];
            char filename[MAX_FILENAME];
            char target[MAX_USERNAME];
            if (sscanf(input, "ADDACCESS %3s %255s %63s", mode_flag, filename, target) == 3) {
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
            if (sscanf(input, "REMACCESS %255s %63s", filename, target) == 2) {
                handle_remaccess(filename, target);
            } else {
                printf("Usage: REMACCESS <filename> <username>\n");
            }
        } else if (strcmp(cmd, "READ") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "READ %255s", filename) == 1) {
                handle_read(filename);
            } else {
                printf("Usage: READ <filename>\n");
            }
        } else if (strcmp(cmd, "WRITE") == 0) {
            char filename[MAX_FILENAME];
            int sentence_index = 0;
            if (sscanf(input, "WRITE %255s %d", filename, &sentence_index) == 2) {
                handle_write(filename, sentence_index);
            } else {
                printf("Usage: WRITE <filename> <sentence_number>\n");
            }
        } else if (strcmp(cmd, "STREAM") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "STREAM %255s", filename) == 1) {
                handle_stream(filename);
            } else {
                printf("Usage: STREAM <filename>\n");
            }
        } else if (strcmp(cmd, "UNDO") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "UNDO %255s", filename) == 1) {
                handle_undo(filename);
            } else {
                printf("Usage: UNDO <filename>\n");
            }
        } else if (strcmp(cmd, "DELETE") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "DELETE %255s", filename) == 1) {
                handle_delete(filename);
            } else {
                printf("Usage: DELETE <filename>\n");
            }
        } else if (strcmp(cmd, "EXEC") == 0) {
            char filename[MAX_FILENAME];
            if (sscanf(input, "EXEC %255s", filename) == 1) {
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
