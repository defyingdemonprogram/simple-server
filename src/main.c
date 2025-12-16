#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <ctype.h>

#include "logging.h"
#define PORT 6969
#define BUFFER_SIZE (4*1024)

// Helper to get Content-Length case-insensitively
static long get_content_length(const char *headers) {
    const char *key = "Content-Length:";
    const char *p = headers;
    while (*p) {
        if (strncasecmp(p, key, 15) == 0) {
            return strtol(p + 15, NULL, 10);
        }
        p++;
    }
    return 0;
}

typedef struct {
    int socket;
    struct sockaddr_in addr;
} ClientInfo;

static int should_close_connection(const char *req) {
    // Case-insensitive search
    const char *p = req;
    while(*p) {
        if (strncasecmp(p, "Connection:", 11)==0) {
            p += 11;
            while (*p == ' ') p++;
            if (strncasecmp(p, "close", 5)==0)
                return 1;
        }
        p++;
    }
    return 0; // keep-alive by default
}

static int url_decode(char *dst, const char *src, size_t max) {
    size_t i = 0, j = 0;
    while (src[i] && j < max - 1) {
        if (src[i] == '%' && isxdigit(src[i+1]) && isxdigit(src[i+2])) {
            char hex[3] = {src[i+1], src[i+2], 0};
            dst[j++] = strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = 0;
    return j;
}

static int is_safe_path(const char *path) {
    // Prevent "../", "/..", "....", anything containing ".."
    return strstr(path, "..") == NULL;
}


static const char *mime_type(const char *path) {
    // get the last string after dot
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";

    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".js") == 0) return "application/javascript";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0) return "image/jpeg";
    if (strcmp(dot, ".jpeg") == 0) return "image/jpeg";

    return "application/octet-stream";
}

static int send_all(int sock, const void *buf, size_t len) {
    const char *p = buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, 0);
        if (n <= 0) return -1; // error
        sent += n;
    }
    return 0;
}


void serve_file(int client_socket, struct sockaddr_in *client_addr, const char *raw_path) {
    char decoded[256];
    url_decode(decoded, raw_path, sizeof(decoded));

    // Normalize leading slashes
    const char *path = decoded;
    while (*path == '/') path++;
 
    // Prevent simple directory traversal
    if (!is_safe_path(path)) {
        const char *bad = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send_all(client_socket, bad, strlen(bad));
        log_request(client_addr, "GET", raw_path, 400);
        return;
    }

    char full_path[512];
    if (strcmp(raw_path, "/") == 0) {
        snprintf(full_path, sizeof(full_path), "./public/index.html");
    } else {
        snprintf(full_path, sizeof(full_path), "./public/%s", path);
    }

    FILE *file = fopen(full_path, "rb");
    if (!file) {
        const char *not_found = 
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n\r\n"
            "<h1>404 - File Not Found</h1>";
        send_all(client_socket, not_found, strlen(not_found));
        log_request(client_addr, "GET", raw_path, 404);
        return;
    }

    // Determine MIME type
    const char *mime = mime_type(full_path);

    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        mime, size);

    send_all(client_socket, header, strlen(header));

    char file_buffer[BUFFER_SIZE];
    size_t bytes;
    while((bytes = fread(file_buffer, 1, BUFFER_SIZE, file)) > 0) {
        if (send_all(client_socket, file_buffer, bytes) < 0) break;
    }

    fclose(file);
    log_request(client_addr, "GET", raw_path, 200);
}

void handle_post_request(int client_socket, struct sockaddr_in *client_addr, const char *path, char *buffer, int initial_len) {
    // 1. Find end of headers
    char *body_start = strstr(buffer, "\r\n\r\n");
    if (!body_start) {
        // Should practically not happen if we parsed method, but safest
        return; 
    }
    body_start += 4; // Skip \r\n\r\n

    // 2. Parse Content-Length
    long content_len = get_content_length(buffer);
    if (content_len <= 0) {
        const char *resp = "HTTP/1.1 411 Length Required\r\nContent-Length: 0\r\n\r\n";
        send_all(client_socket, resp, strlen(resp));
        log_request(client_addr, "POST", path, 411);
        return;
    }

    // 3. Read body
    char *body = malloc(content_len + 1);
    if (!body) {
        perror("malloc");
        return;
    }

    // Copy what we might have already read
    int header_len = body_start - buffer;
    int already_read = initial_len - header_len;
    
    if (already_read > 0) {
        if (already_read > content_len) already_read = content_len; // Safety clamp
        memcpy(body, body_start, already_read);
    }

    int total_body_read = already_read;
    while (total_body_read < content_len) {
        int left = content_len - total_body_read;
        int n = recv(client_socket, body + total_body_read, left, 0);
        if (n <= 0) break;
        total_body_read += n;
    }
    body[total_body_read] = '\0';

    // 4. Process Body (Echo for now)
    printf("[POST Data] %s\n", body);

    char response[1024];
    int resp_len = snprintf(response, sizeof(response), 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "Received: %s",
        (int)(10 + strlen(body)), body); // "Received: " is 10 chars

    send_all(client_socket, response, resp_len);
    log_request(client_addr, "POST", path, 200);
    free(body);
}

void *handle_client(void *arg) {
    ClientInfo *cinfo = (ClientInfo *) arg;
    int client_socket = cinfo->socket;
    struct sockaddr_in client_addr = cinfo->addr;

    char buffer[BUFFER_SIZE];
    while(1) {
        int bytes = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            break; // client closed or error
        }

        buffer[bytes] = '\0';

        char method[10], path[256];
        if (sscanf(buffer, "%9s %255s", method, path) != 2) {
            const char *bad = "HTTP/1.1 400 Bad Request\r\n"
                              "Connection: close\r\n"
                              "\r\n";
            send_all(client_socket, bad, strlen(bad));
            log_request(&client_addr, "UNKNOWN", "UNKNOWN", 400);
            break;
        }

        int close_conn = should_close_connection(buffer);

        if (strcmp(method, "GET") == 0) {
            serve_file(client_socket, &client_addr, path);
        } else if (strcmp(method, "POST") == 0) {
            handle_post_request(client_socket, &client_addr, path, buffer, bytes);
        } else {
            const char *bad = "HTTP/1.1 405 Method Not Allowed\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n";
            send_all(client_socket, bad, strlen(bad));
            log_request(&client_addr, method, path, 405);
        }

        if (close_conn) {
            break;
        }
    }

    close(client_socket);
    free(cinfo); // free memory allocated for client info
    return NULL;
}


int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    printf("Server FD: %d\n", server_fd);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT)
    };

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
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_socket < 0) {
            perror("accept");
            continue;
        }

        // Allocate memory for client info
        ClientInfo *cinfo = malloc(sizeof(ClientInfo));
        cinfo->socket = client_socket;
        cinfo->addr = client_addr;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, cinfo) != 0) {
            perror("pthread_create");
            close(client_socket);
            free(cinfo);
            continue;
        }
        pthread_detach(tid); // Detach thread so resourxexs are freed automatically`
    }
    return 0;
}
