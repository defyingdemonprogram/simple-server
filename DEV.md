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

