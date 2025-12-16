## Socket

In network programming, a **socket** is an endpoint for communication between two machines over a network. It allows programs to send and receive data to or from remote servers or clients. In this context, the code implements a simple HTTP server that uses sockets to communicate with clients and serve files—typically over the TCP/IP protocol.

In Linux, **everything is represented as a file**, and sockets follow the same philosophy. A socket is associated with a **file descriptor**, which the operating system uses to track and manage I/O operations. When a program creates a socket, the kernel returns a file descriptor that the program can use with standard system calls such as `read()`, `write()`, and `close()`.

To specify a socket’s address, the structure `sockaddr_in` is used (for IPv4):

```c
struct sockaddr_in {
    sa_family_t    sin_family;   // Address family (AF_INET for IPv4)
    in_port_t      sin_port;     // Port number (in network byte order)
    struct in_addr sin_addr;     // IP address (in network byte order)
};
```

Because different machine architectures store multibyte values differently (**endianness**), network protocols use **network byte order**, which is big-endian. Therefore, we must convert values such as port numbers and IP addresses when moving between host and network representations:

* `htons()` — host-to-network short (for port numbers)
* `htonl()` — host-to-network long (for IP addresses)
* `ntohs()` / `ntohl()` — network-to-host conversions, used to obtain human-readable or host-order values

These conversions ensure that machines with different architectures can communicate reliably over the network.

The process of creating a socket connection on the server side typically involves:

1. **Creating the socket**
2. **Setting socket options (optional but recommended)**
3. **Binding** the socket to an IP address and port
4. **Listening** for incoming connections
5. **Accepting** client connections

### Socket Family and Type

When creating a socket, you specify its **domain (family)**, **type**, and **protocol**:

```c
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
```

* `AF_INET` — IPv4 Internet protocols
* `SOCK_STREAM` — reliable, connection-oriented TCP
* `0` — default protocol for the given type

You can also set socket options using `setsockopt()` to modify socket behavior. For example:

```c
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

* `SOL_SOCKET` — socket-level options
* `SO_REUSEADDR` — allows the socket to reuse the address (prevents "address already in use" errors)


### HTTP Methods & POST Requests

HTTP defines several methods (verb) that indicate the desired action to be performed on the identified resource.
- **GET**: Requests a representation of the specified resource. Requests using GET should only retrieve data.
- **POST**: Used to submit an entity to the specified resource, often causing a change in state or side effects on the server.

#### Handling POST Requests
When handling `POST` requests, the server must read data sent by the client (the **request body**). Unlike headers, which are terminated by `\r\n\r\n`, the body does not have a specific terminator. Instead, the `Content-Length` header specifies the number of bytes in the body.

**Key Steps:**
1.  **Parse Headers**: Look for `Content-Length`.
2.  **Locate Body Start**: The body starts after the double CRLF (`\r\n\r\n`).
3.  **Read Body**: Read exactly `Content-Length` bytes. Since TCP is a stream, the initial `read()` might contain only part of the body (or even just headers), so we may need to call `recv()` multiple times until we have the full body.

```c
// Example Logical Flow
char *body_start = strstr(buffer, "\r\n\r\n") + 4;
int body_len = atoi(get_header_value(buffer, "Content-Length"));
int already_read = total_read - (body_start - buffer);
int remaining = body_len - already_read;

// recv loop for remaining bytes...
```

### File System & Directory Listing

To serve files or list directory contents, the server interacts with the operating system's file system using specific system calls.

1.  **stat()**: Used to determine the status of a file path. It tells us if a path exists and whether it is a regular file (`S_ISREG`) or a directory (`S_ISDIR`).
2.  **opendir() / readdir() / closedir()**: These functions allow the program to iterate over the contents of a directory.
    -   `opendir` opens a directory stream.
    -   `readdir` returns the next entry in the directory (a `struct dirent`), which contains the filename.

#### Dynamic Content Generation
When listing a directory, the server **dynamically generates** an HTML page. Instead of reading a static file from disk, it constructs a string containing the HTML structure (e.g., `<ul><li><a href="...">filename</a></li>...</ul>`) and sends that as the response body.

### Concurrency & Thread Pool

To handle multiple clients simultaneously, a server needs **concurrency**.
-   **Threads vs. Processes**: Threads are lighter-weight than processes and share the same memory space, making data sharing easier (but requiring synchronization).
-   **Race Conditions**: When multiple threads access shared resources (like a log file or a global variable), they might interfere with each other. **Mutexes** (Mutual Exclusion locks) are used to protect these critical sections.
-   **Condition Variables**: These allow threads to sleep until a specific condition is met (e.g., "there is a task in the queue"). This avoids "busy-waiting" which wastes CPU.

#### Thread Pool Pattern
Creating a thread for every single request (`pthread_create` per client) is expensive. A **Thread Pool** is a pattern where:
1.  A fixed number of worker threads are created at startup.
2.  They wait for work in a shared **Queue**.
3.  The main thread accepts connections and pushes them into the queue.
4.  A worker wakes up, processes the request, and goes back to waiting.
This limits the resource usage and provides predictable performance.
