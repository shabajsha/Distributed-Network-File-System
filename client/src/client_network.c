//AI code starts
#include "client_network.h"

int connect_to_nm(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(NM_PORT);

    if (inet_pton(AF_INET, NM_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to Name Server failed");
        close(sock);
        return -1;
    }

    return sock;
}

int connect_to_ss(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to Storage Server failed");
        close(sock);
        return -1;
    }

    return sock;
}

void send_message(int fd, const char *message) {
    size_t len = strlen(message);
    char *payload = (char *)malloc(len + 2);
    if (!payload) {
        send(fd, message, len, 0);
        return;
    }
    memcpy(payload, message, len);
    payload[len] = '\n';
    payload[len + 1] = '\0';
    send(fd, payload, len + 1, 0);
    free(payload);
}

char *receive_message(int fd) {
    char *buffer = (char *)malloc(BUFFER_SIZE);
    if (!buffer) {
        return NULL;
    }

    int bytes_read = recv(fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read <= 0) {
        free(buffer);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    return buffer;
}
//AI code ends