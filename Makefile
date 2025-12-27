# Makefile for LangOS Document Collaboration System

CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -pthread

# Directories
CLIENT_DIR = client
NM_DIR = name_server
SS_DIR = storage_server

# Output binaries
CLIENT_BIN = $(CLIENT_DIR)/client
NM_BIN = $(NM_DIR)/nm
SS_BIN = $(SS_DIR)/ss

# Client directories
CLIENT_SRC_DIR = $(CLIENT_DIR)/src
CLIENT_INC_DIR = $(CLIENT_DIR)/include
CLIENT_OBJ_DIR = $(CLIENT_DIR)/obj

# Name server directories
NM_SRC_DIR = $(NM_DIR)/src
NM_INC_DIR = $(NM_DIR)/include
NM_OBJ_DIR = $(NM_DIR)/obj

# Storage server directories
SS_SRC_DIR = $(SS_DIR)/src
SS_INC_DIR = $(SS_DIR)/include
SS_OBJ_DIR = $(SS_DIR)/obj

# Storage server object files (in obj/ directory)
SS_OBJS = $(SS_OBJ_DIR)/ss_main.o $(SS_OBJ_DIR)/ss_file_ops.o $(SS_OBJ_DIR)/ss_locking.o \
          $(SS_OBJ_DIR)/ss_session.o $(SS_OBJ_DIR)/ss_utils.o $(SS_OBJ_DIR)/ss_handlers.o \
          $(SS_OBJ_DIR)/ss_write_handlers.o $(SS_OBJ_DIR)/ss_network.o

# Client object files
CLIENT_OBJS = $(CLIENT_OBJ_DIR)/client_main.o $(CLIENT_OBJ_DIR)/client_network.o \
			  $(CLIENT_OBJ_DIR)/client_commands.o $(CLIENT_OBJ_DIR)/client_utils.o

# Name server object files
NM_OBJS = $(NM_OBJ_DIR)/nm_main.o $(NM_OBJ_DIR)/nm_cache.o $(NM_OBJ_DIR)/nm_handlers.o \
		  $(NM_OBJ_DIR)/nm_logging.o $(NM_OBJ_DIR)/nm_metadata.o $(NM_OBJ_DIR)/nm_network.o

# Targets
all: $(CLIENT_BIN) $(NM_BIN) $(SS_BIN)

$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $(CLIENT_BIN) $(CLIENT_OBJS) $(LDFLAGS)

$(NM_BIN): $(NM_OBJS)
	$(CC) $(CFLAGS) -o $(NM_BIN) $(NM_OBJS) $(LDFLAGS)

# Modular storage server compilation
$(SS_BIN): $(SS_OBJS)
	$(CC) $(CFLAGS) -o $(SS_BIN) $(SS_OBJS) $(LDFLAGS)

# Compile storage server source files with include path
$(SS_OBJ_DIR)/%.o: $(SS_SRC_DIR)/%.c
	$(CC) $(CFLAGS) -I$(SS_INC_DIR) -c -o $@ $<

# Compile client source files with include path
$(CLIENT_OBJ_DIR)/%.o: $(CLIENT_SRC_DIR)/%.c
	$(CC) $(CFLAGS) -I$(CLIENT_INC_DIR) -c -o $@ $<

# Compile name server source files with include path
$(NM_OBJ_DIR)/%.o: $(NM_SRC_DIR)/%.c
	$(CC) $(CFLAGS) -I$(NM_INC_DIR) -c -o $@ $<

# Convenience aliases
client: $(CLIENT_BIN)
nm: $(NM_BIN)
ss: $(SS_BIN)

clean:
	rm -f $(CLIENT_BIN) $(NM_BIN) $(SS_BIN)
	rm -f $(CLIENT_OBJ_DIR)/*.o
	rm -f $(NM_OBJ_DIR)/*.o
	rm -f $(SS_OBJ_DIR)/*.o
	rm -rf $(NM_DIR)/logs/
	rm -rf $(SS_DIR)/logs/

help:
	@echo "LangOS Document Collaboration System - Build & Run Commands"
	@echo ""
	@echo "BUILD COMMANDS:"
	@echo "  make all        - Build all components (client, name server, storage server)"
	@echo "  make client     - Build only the client"
	@echo "  make nm         - Build only the name server"
	@echo "  make ss         - Build only the storage server"
	@echo "  make clean      - Remove all binaries and logs"
	@echo ""
	@echo "RUN COMMANDS (Single Machine):"
	@echo "  make run-nm     - Start name server (default port 9000)"
	@echo "  make run-client - Start client (connects to localhost:9000)"
	@echo "  make run-ss     - Start storage server on port 9100"
	@echo ""
	@echo "RUN COMMANDS (Multiple Machines):"
	@echo "  Name Server:    ./name_server/nm"
	@echo "  Client:         ./client/client <name_server_ip>"
	@echo "  Storage Server: ./storage_server/ss <port> <name_server_ip>"
	@echo ""
	@echo "EXAMPLES:"
	@echo "  # On machine 1 (192.168.1.10) - Run name server:"
	@echo "  ./name_server/nm"
	@echo ""
	@echo "  # On machine 2 - Run storage server:"
	@echo "  ./storage_server/ss 9100 192.168.1.10"
	@echo ""
	@echo "  # On machine 3 - Run client:"
	@echo "  ./client/client 192.168.1.10"
	
run-nm:
	./$(NM_BIN)

run-client:
	./$(CLIENT_BIN)

run-ss:
	./$(SS_BIN) 9100

run-ss-port:
	@if [ -z "$(PORT)" ]; then \
		echo "Usage: make run-ss-port PORT=<port_number>"; \
	else \
		./$(SS_BIN) $(PORT); \
	fi

.PHONY: all clean run-nm run-client run-ss client nm ss help
