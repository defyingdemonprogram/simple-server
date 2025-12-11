#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>

#define PORT 6969
#define BUFFER_SIZE (4*1024)

void print_sockaddr(struct sockaddr_in *addr) {
    char ip[INET_ADDRSTRLEN];

    // Convert the binary IP to string
    inet_ntop(AF_INET, &(addr->sin_addr), ip, INET_ADDRSTRLEN);

    // Convert port from network byte order to host byte order
    int port = ntohs(addr->sin_port);

    printf("sockaddr_in values:\n");
    printf("    Family: AF_INET (%d)\n", addr->sin_family);
    printf("    IP: %s\n", ip);
    printf("    Port: %d\n", port);
}

void serve_file(int client_socket, const char *raw_path) {
    int is_root = strcmp(raw_path, "/") == 0;

    // Remove leading slash except for "/"
    const char *path = raw_path;
    if(path[0] == '/' && !is_root) {
        path++;
    }
    
    // Prevent simple directory traversal
    if (strstr(path, "..")) {
        const char *bad = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(client_socket, bad, strlen(bad), 0);
        return;
    }

    char full_path[256];
    snprintf(full_path, sizeof(full_path), "./public/%s",
    is_root ? "index.html" : path);

    printf("[INFO]: Serving file: %s\n", full_path);

    FILE *file = fopen(full_path, "r");
    if (!file) {
        const char *not_found = 
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n\r\n"
            "<h1>404 - File Not Found</h1>";
        send(client_socket, not_found, strlen(not_found), 0);
        return;
    }

    const char *header = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n\r\n";

    send(client_socket, header, strlen(header), 0);

    char file_buffer[BUFFER_SIZE];
    size_t bytes;
    while((bytes = fread(file_buffer, 1, BUFFER_SIZE, file)) > 0) {
        send(client_socket, file_buffer, bytes, 0);
    }

    fclose(file);
}

int main() {
    int server_fd, client_socket;
    struct sockaddr_in server_addr, client_addr;
    char buffer[BUFFER_SIZE];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    printf("Server FD: %d\n", server_fd);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    print_sockaddr(&server_addr);
    printf("Server running on http://localhost:%d\n", PORT);
    while (1) {
        socklen_t client_len = sizeof(client_addr);
        client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_socket < 0) {
            perror("accept");
            continue;
        }

        int bytes = read(client_socket, buffer, BUFFER_SIZE - 1);
        if (bytes <= 0) {
            close(client_socket);
            continue;
        }

        buffer[bytes] = '\0';  // safe null-termination

        char method[10], path[256];
        sscanf(buffer, "%9s %255s", method, path);

        printf("Client %02d | Request: %s %s\n", client_socket, method, path);

        if (strcmp(method, "GET") == 0) {
            serve_file(client_socket, path);
        } else {
            const char *bad = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
            send(client_socket, bad, strlen(bad), 0);
        }

        close(client_socket);
    }
    
    return 0;
}
