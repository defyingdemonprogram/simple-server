#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <time.h>
#include "logging.h"

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

void log_request(struct sockaddr_in *client_addr, const char *method, const char *path, int status_code) {
    FILE *log_file = fopen("access.log", "a");
    if (!log_file) {
        perror("fopen log");
        return;
    }

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), ip, INET_ADDRSTRLEN);
    int port = ntohs(client_addr->sin_port);

    // Get current time
    time_t now = time(NULL);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%d/%b/%Y:%H:%M:%S %z", localtime(&now));

    // Format the log line
    char log_line[512];
    snprintf(log_line, sizeof(log_line), "[%s] %s:%d \"%s %s\" %d\n", time_str, ip, port, method, path, status_code);

    // Write to log file
    fputs(log_line, log_file);
    fclose(log_file);

    // Display in terminal
    printf("%s", log_line);
}
