#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

#include "logging.h"
#define BUFFER_SIZE (4*1024)
#define THREAD_POOL_SIZE 8
#define QUEUE_SIZE 256

int SERVER_PORT = 6969;
char SERVER_ROOT[256] = "./public";

void load_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("Config file not found, using defaults.\n");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        // Remove newline
        line[strcspn(line, "\n")] = 0;
        
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0; // split key and value
            char *key = line;
            char *value = eq + 1;
            
            if (strcmp(key, "PORT") == 0) {
                SERVER_PORT = atoi(value);
            } else if (strcmp(key, "ROOT_DIR") == 0) {
                strncpy(SERVER_ROOT, value, sizeof(SERVER_ROOT) - 1);
            }
        }
    }
    fclose(file);
    printf("Loaded Config: PORT=%d, ROOT=%s\n", SERVER_PORT, SERVER_ROOT);
}

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

typedef struct {
    void *(*function)(void *);
    void *arg;
} Task;

Task task_queue[QUEUE_SIZE];
int queue_head = 0;
int queue_tail = 0;
int queue_count = 0;

pthread_mutex_t queue_lock;
pthread_cond_t cond_not_empty;
pthread_cond_t cond_not_full;

void init_thread_pool();
void submit_task(Task task);
void *worker_thread(void *arg);

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

static void serve_directory(int client_socket, const char *path, const char *full_path) {
    DIR *d;
    struct dirent *dir;
    d = opendir(full_path);
    if (!d) {
        perror("opendir");
        // Fallback to 404 or 403
        return;
    }

    char body[1024 * 16]; // 16KB buffer for directory listing
    strcpy(body, "<html><head><title>Directory Listing</title></head><body>");
    strcat(body, "<h1>Directory Listing</h1><ul>");

    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0) continue;
        
        char line[1024];
        // Ensure we handle root path correctly for links
        const char *prefix = (strcmp(path, "/") == 0) ? "" : path;
        // Should ideally escape quotes etc.
        snprintf(line, sizeof(line), "<li><a href=\"%s/%s\">%s</a></li>", prefix, dir->d_name, dir->d_name);
        
        // Simple buffer check
        if (strlen(body) + strlen(line) < sizeof(body) - 50) {
            strcat(body, line);
        } else {
            strcat(body, "<li>... truncated ...</li>");
            break;
        }
    }
    closedir(d);

    strcat(body, "</ul></body></html>");

    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        strlen(body));

    send_all(client_socket, header, strlen(header));
    send_all(client_socket, body, strlen(body));
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
        snprintf(full_path, sizeof(full_path), "%s", SERVER_ROOT);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", SERVER_ROOT, path);
    }

    struct stat st;
    if (stat(full_path, &st) < 0) {
         // File not found
        const char *not_found = 
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n\r\n"
            "<h1>404 - File Not Found</h1>";
        send_all(client_socket, not_found, strlen(not_found));
        log_request(client_addr, "GET", raw_path, 404);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        // Check for index.html
        char index_path[1024];
        snprintf(index_path, sizeof(index_path), "%s/index.html", full_path);
        if (stat(index_path, &st) == 0) {
            // index.html exists, serve it
            strcpy(full_path, index_path);
        } else {
            // Serve directory listing
            serve_directory(client_socket, raw_path, full_path);
            log_request(client_addr, "GET", raw_path, 200);
            return;
        }
    }

    FILE *file = fopen(full_path, "rb");
    if (!file) {
        // Should have been caught by stat, but race condition or permission possible
        perror("fopen");
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
    load_config("server.conf");

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
        .sin_port = htons(SERVER_PORT)
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
    print_sockaddr(&server_addr);
    printf("Server running on http://localhost:%d\n", SERVER_PORT);

    // Fix: Move load_config to top of main.

    init_thread_pool();

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

        Task task;
        task.function = handle_client;
        task.arg = cinfo;

        submit_task(task);
    }
    return 0;
}

void init_thread_pool() {
    pthread_mutex_init(&queue_lock, NULL);
    pthread_cond_init(&cond_not_empty, NULL);
    pthread_cond_init(&cond_not_full, NULL);

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, worker_thread, NULL) != 0) {
            perror("pthread_create");
        }
        // No detach needed if we just let them run forever, but detach is cleaner if we had shutdown logic
        pthread_detach(tid); 
    }
}

void submit_task(Task task) {
    pthread_mutex_lock(&queue_lock);
    while (queue_count == QUEUE_SIZE) {
        pthread_cond_wait(&cond_not_full, &queue_lock);
    }
    
    task_queue[queue_tail] = task;
    queue_tail = (queue_tail + 1) % QUEUE_SIZE;
    queue_count++;
    
    pthread_cond_signal(&cond_not_empty);
    pthread_mutex_unlock(&queue_lock);
}

void *worker_thread(void *arg) {
    (void)arg; // Unused
    while (1) {
        Task task;
        
        pthread_mutex_lock(&queue_lock);
        while (queue_count == 0) {
            pthread_cond_wait(&cond_not_empty, &queue_lock);
        }
        
        task = task_queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_SIZE;
        queue_count--;
        
        pthread_cond_signal(&cond_not_full);
        pthread_mutex_unlock(&queue_lock);
        
        // Execute task
        task.function(task.arg);
    }
    return NULL;
}
