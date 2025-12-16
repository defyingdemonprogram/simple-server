#ifndef LOGGING_H_
#define LOGGING_H_

void log_request(struct sockaddr_in *addr, const char *method, const char *path, int status);
void print_sockaddr(struct sockaddr_in *addr);

#endif // LOGGING_H_
