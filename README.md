# Mini HTTP Server in C

A minimal HTTP server written in C that uses low-level UNIX sockets to serve static files. This project demonstrates the fundamentals of networking, socket programming, and simple HTTP protocol handling.

### Getting Started

1. **Build the project**

```bash
make
```

2. **Run the server**

```bash
./bin/server
```

3. **Access the server**
   Open your browser and navigate to: [http://localhost:6969](http://localhost:6969)

### Testing Features

#### 1. POST Request
You can test the POST request handling using `curl`:

```bash
curl -v POST -d "Hello Server" http://localhost:6969/submit
```

The server should respond echoing your data.

#### 2. Directory Listing
To test directory listing, create a folder inside `public/` and access it:

```bash
mkdir -p public/test
touch public/test/a.txt public/test/b.png
```

Then navigate to [http://localhost:6969/test/](http://localhost:6969/test/) in your browser or use curl:

```bash
curl http://localhost:6969/test/
```

### References

* [Network Socket – Wikipedia](https://en.wikipedia.org/wiki/Network_socket)
* [Socket – Linux man page](https://www.man7.org/linux/man-pages/man2/socket.2.html)
* [Socket Address Structures – man7.org](https://www.man7.org/linux/man-pages/man7/socket.7.html)
