# Asynchronous Web Server in C

## Overview

This project showcases a high-performance asynchronous web server implemented in C, utilizing advanced Linux features such as `libaio`, `epoll`, and `eventfd` for efficient I/O operations. Designed with a focus on non-blocking, event-driven architecture, this server is capable of handling a high volume of concurrent connections, making it a prime example of low-level system programming in a modern Linux environment.

## Features

- **Asynchronous File I/O:** Leverages `libaio` for non-blocking file reads, allowing the server to serve dynamic content efficiently.
- **Event-Driven Networking:** Utilizes `epoll` for scalable event notification, enabling the server to manage thousands of simultaneous connections.
- **Efficient Data Transfer:** Employs `sendfile` for zero-copy transfers of static content directly from disk to network, minimizing CPU usage.
- **Robust HTTP Parsing:** Integrates a lightweight yet comprehensive HTTP parser to handle requests accurately.
- **Time and Date Handling:** Implements RFC-compliant date formatting for HTTP headers, enhancing compatibility with web standards.

## Getting Started

### Prerequisites

- Linux environment
- GCC or Clang compiler
- libaio library
- Basic knowledge of network programming in C

### Compilation

To compile the server, use the following command:


### Running the Server

To run the server, execute:

./async_web_server


The server listens on port 8080 by default. Adjust the `AWS_LISTEN_PORT` macro in `aws.h` to change the default port.

## Usage

Once running, the server can serve static and dynamic content from specified directories. Static content is served from a directory named `static` and dynamic content from `dynamic`.

## Testing

You can test the server's functionality using any HTTP client, such as curl:

curl http://localhost:8080/static/index.html

## Design and Implementation

### Architecture Overview

- **Main Loop:** The server uses an epoll-based loop to efficiently multiplex incoming connections and I/O events.
- **Asynchronous I/O:** Dynamic content requests are handled asynchronously using libaio, with eventfd for notification.
- **Connection Management:** Each client connection is represented by a `struct connection`, facilitating organized management of state and data.

### Detailed Workflow

1. **Initialization:** Set up epoll instance and listening socket.
2. **Accepting Connections:** New connections are non-blocking and added to epoll.
3. **Request Handling:** Asynchronous reads for dynamic content and efficient `sendfile` for static content.
4. **Response Preparation:** HTTP headers are crafted, with proper date and last-modified headers.
5. **Cleanup:** Resources are freed and connections are closed gracefully.

## Contributing

Contributions are welcome! For major changes, please open an issue first to discuss what you would like to change. Please ensure to update tests as appropriate.

## License

This project is licensed under the BSD-3-Clause License - see the LICENSE file for details.
