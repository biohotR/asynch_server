// SPDX-License-Identifier: BSD-3-Clause

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libaio.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#include "aws.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

int min_num(int a, int b) { return a < b ? a : b; }

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

// Function to format the date
void format_date(time_t time, char *buf, size_t size)
{
	struct tm tm;

	strftime(buf, size, "%a, %d %b %Y %H:%M:%S GMT", gmtime_r(&time, &tm));
}

// Function to get the last modified date
int last_mod_date(const char *path, char *buf, size_t size)
{
	struct stat file_stat;

	if (stat(path, &file_stat) == 0) {
		format_date(file_stat.st_mtime, buf, size);
		return 0;
	}
	return -1;
}

// Function to prepare the header for the response
static void connection_prepare_send_reply_header(struct connection *conn)
{
	char date[50];
	char last_modified_date[50];
	time_t now = time(NULL);

	if (!conn)
		return;

	// Get current date
	format_date(now, date, sizeof(date));
	// Get the last modified date
	if (last_mod_date(conn->filename, last_modified_date, sizeof(last_modified_date)) == -1) {
		perror("last_modified_date");
		return;
	}

	// Create the header
	const char *header_fmt = "HTTP/1.1 200 OK\r\n"
							 "Date: %s\r\n"
							 "Server: Apache/2.2.9\r\n"
							 "Last-Modified: %s\r\n"
							 "Accept-Ranges: bytes\r\n"
							 "Vary: Accept-Encoding\r\n"
							 "Connection: close\r\n"
							 "Content-Type: text/html\r\n"
							 "Content-Length: %ld\r\n\r\n";

	// Clear the buffer
	memset(conn->send_buffer, 0, BUFSIZ);
	// Write the header in the buffer
	snprintf(conn->send_buffer, BUFSIZ, header_fmt, date, last_modified_date, conn->file_size);
	conn->send_len = strlen(conn->send_buffer);
}

// Function to prepare send 404
static void connection_prepare_send_404(struct connection *conn)
{
	if (!conn)
		return;

	// Create the header
	const char *header_fmt = "HTTP/1.1 404 Not Found\r\n"
							 "Content-Type: text/html\r\n"
							 "Connection: close\r\n"
							 "\r\n";

	// Clear the buffer
	memset(conn->send_buffer, 0, BUFSIZ);
	// Write the header in the buffer
	snprintf(conn->send_buffer, BUFSIZ, "%s", header_fmt);
	conn->send_len = strlen(conn->send_buffer);
}

// Function to get the type of the resource
static enum resource_type
connection_get_resource_type(struct connection *conn)
{
	if (!conn)
		return RESOURCE_TYPE_NONE;

	// If the request path contains the static folder
	if (strstr(conn->request_path, AWS_REL_STATIC_FOLDER)) {
		conn->filename[0] = '.';
		strcat(conn->filename, conn->request_path);
		return RESOURCE_TYPE_STATIC;
		// If the request path contains the dynamic folder
	} else if (strstr(conn->request_path, AWS_REL_DYNAMIC_FOLDER)) {
		conn->filename[0] = '.';
		strcat(conn->filename, conn->request_path);
		return RESOURCE_TYPE_DYNAMIC;
	}
	return RESOURCE_TYPE_NONE;
}

// Function to create a new connection
struct connection *connection_create(int sockfd)
{
	// Allocate memory for the connection
	struct connection *conn = malloc(sizeof(struct connection));

	if (!conn) {
		perror("malloc");
		return NULL;
	}

	// Initialize the connection
	memset(conn, 0, sizeof(struct connection));
	conn->sockfd = sockfd;
	conn->state = STATE_INITIAL;
	conn->fd = -1;
	conn->eventfd = -1;
	memset(conn->send_buffer, 0, BUFSIZ);
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->request_path, 0, BUFSIZ);

	return conn;
}

// Function to start the async io
void connection_start_async_io(struct connection *conn)
{
	int rc;

	if (!conn)
		return;

	// Initialize the eventfd
	conn->eventfd = eventfd(0, EFD_NONBLOCK);
	memset(&conn->iocb, 0, sizeof(conn->iocb));
	// Calculate the size to read
	int read_size = min_num(BUFSIZ, conn->file_size - conn->file_pos);

	conn->send_len = read_size;
	if (conn->fd < 0)
		return;

	ctx = conn->ctx;
	// Create the context
	rc = io_setup(1, &ctx);
	// Prepare the read
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, read_size,
				  conn->file_pos);
	io_set_eventfd(&conn->iocb, conn->eventfd);
	conn->piocb[0] = &conn->iocb;
	// Submit
	rc = io_submit(ctx, 1, conn->piocb);
	if (rc < 0)
		return;
	// Add the eventfd to the epoll
	rc = w_epoll_add_ptr_in(epollfd, conn->eventfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

// Function to continue the async io
void connection_continue_async_io(struct connection *conn)
{
	if (!conn)
		return;
	// Calculate the size to read
	int read_size = min_num(BUFSIZ, conn->file_size - conn->file_pos);

	conn->send_len = read_size;
	// Prepare the read
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, read_size,
				  conn->file_pos);
	io_set_eventfd(&conn->iocb, conn->eventfd);
	conn->piocb[0] = &conn->iocb;
	// Submit
	int rc = io_submit(ctx, 1, conn->piocb);
	// If the submit failed, restart the async io connection
	if (rc < 0)	{
		connection_complete_async_io(conn);
		connection_start_async_io(conn);
		conn->state = STATE_ASYNC_ONGOING;

		return;
	}

	conn->state = STATE_SENDING_DATA;
	conn->file_pos += read_size;
}

// Function to complete the async io
void connection_complete_async_io(struct connection *conn)
{
	if (!conn)
		return;

	// Destroy the context
	io_destroy(ctx);
	conn->ctx = NULL;
	ctx = NULL;
	// Close the eventfd
	close(conn->eventfd);
	conn->eventfd = -1;
	conn->state = STATE_SENDING_DATA;
	// Remove the eventfd from the epoll
	w_epoll_remove_ptr(epollfd, conn->eventfd, conn);
}

// Function to remove a connection
void connection_remove(struct connection *conn)
{
	if (!conn)
		return;

	// If sockfd is valid, remove it from the epoll and close it
	if (conn->sockfd >= 0) {
		w_epoll_remove_fd(epollfd, conn->sockfd);
		close(conn->sockfd);
	}
	// If fd is valid, close it
	if (conn->fd >= 0)
		close(conn->fd);
	// If eventfd is not unll, close it
	if (conn->eventfd >= 0)
		close(conn->eventfd);
	// If ctx is not null, destroy it
	if (conn->ctx)
		io_destroy(conn->ctx);
	// Free memory
	free(conn);
}

void handle_new_connection(void)
{
	struct sockaddr_in add;
	socklen_t addlen = sizeof(add);
	int new_sockfd;
	struct connection *conn;
	int rc;

	// Accept the new connection
	new_sockfd = accept(listenfd, (struct sockaddr *)&add, &addlen);
	DIE(new_sockfd < 0, "accept");

	// Set the socket to non-blocking
	int flags = fcntl(new_sockfd, F_GETFL, 0);

	DIE(flags < 0, "fcntl");
	DIE(fcntl(new_sockfd, F_SETFL, flags | O_NONBLOCK) < 0, "fcntl");
	// Create the connection
	conn = connection_create(new_sockfd);
	if (!conn) {
		close(new_sockfd);
		return;
	}

	// Add the connection to the epoll
	rc = w_epoll_add_ptr_in(epollfd, new_sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");

	// Initialize the http parser
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
}

// Function to check if the request is complete
int is_request_complete(struct connection *conn)
{
	const char *end = "\r\n\r\n";

	if (conn->recv_len >= 4 && strstr(conn->recv_buffer, end) != NULL)
		return 1;
	return 0;
}

int connection_open_file(struct connection *conn)
{
	if (!conn)
		return -1;

	// Open the file
	conn->fd = open(conn->filename, O_RDONLY);
	if (conn->fd < 0) {
		perror("open");
		return -1;
	}

	struct stat buf;
	// Get the file size
	if (fstat(conn->fd, &buf) < 0) {
		perror("fstat");
		close(conn->fd);
		conn->fd = -1;
		return -1;
	}
	conn->file_size = buf.st_size;

	return 0;
}

// Function to parse the header
int parse_header(struct connection *conn)
{
	if (!conn)
		return -1;

	http_parser_settings settings_on_path = {.on_message_begin = 0,
											 .on_header_field = 0,
											 .on_header_value = 0,
											 .on_path = aws_on_path_cb,
											 .on_url = 0,
											 .on_fragment = 0,
											 .on_query_string = 0,
											 .on_body = 0,
											 .on_headers_complete = 0,
											 .on_message_complete = 0};

	// Set the data to the connection
	conn->request_parser.data = conn;
	// Parse the header
	size_t nparsed = http_parser_execute(&conn->request_parser, &settings_on_path,
										 conn->recv_buffer, conn->recv_len);
	// If the parsing failed, return -1
	if (!conn->have_path || nparsed != conn->recv_len)
		return -1;

	return 0;
}

// Function to send static data
enum connection_state connection_send_static(struct connection *conn)
{
	int rc;

	if (!conn || conn->fd < 0)
		return STATE_CONNECTION_CLOSED;

	// Send the file
	off_t offset = conn->file_pos;
	ssize_t sent = sendfile(conn->sockfd, conn->fd, &offset,
							conn->file_size - conn->file_pos);

	// If the send failed, try again
	if (sent < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return STATE_SENDING_DATA;
		perror("sendfile");
		return STATE_CONNECTION_CLOSED;
	}

	conn->file_pos += sent;

	// If the all the file data was sent, then change the state to data sent
	if (conn->file_pos >= conn->file_size) {
		// Update the epoll
		rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
		if (rc < 0) {
			perror("w_epoll_update_ptr_in");
			return STATE_CONNECTION_CLOSED;
		}
		return STATE_DATA_SENT;
	}

	return STATE_SENDING_DATA;
}

// Function to send data
int connection_send_data(struct connection *conn)
{
	if (!conn)
		return -1;

	// If there is no data to send, return 0
	if (conn->send_len == 0)
		return 0;

	// Send the data
	ssize_t bytes_sent =
		send(conn->sockfd, conn->send_buffer + conn->send_pos, conn->send_len, 0);

	// If the send failed, return -1
	if (bytes_sent < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;

		perror("send");
		return -1;
	}

	// Increment the send position and decrement the send length
	conn->send_pos += bytes_sent;
	conn->send_len -= bytes_sent;

	// If all the data was sent, reset the send position
	if (conn->send_len == 0)
		conn->send_pos = 0;

	return bytes_sent;
}

// Function to send dynamic data
int connection_send_dynamic(struct connection *conn)
{
	if (!conn)
		return -1;

	// Send the data
	int bytes_sent = connection_send_data(conn);

	// If the send failed, return -1
	if (bytes_sent < 0) {
		return -1;
		// Else if the data was sent
	} else if (conn->send_len == 0) {
		// If still data to send, continue the async io
		if (conn->file_pos < conn->file_size) {
			conn->state = STATE_ASYNC_ONGOING;
			int rc = w_epoll_update_ptr_in(epollfd, conn->eventfd, conn);

			if (rc < 0) {
				perror("w_epoll_update_ptr_in");
				return STATE_CONNECTION_CLOSED;
			}
			// Else all the data was sent
		} else {
			conn->state = STATE_DATA_SENT;
		}
	}

	return 0;
}

// Function to receive data
void receive_data(struct connection *conn)
{
	if (!conn)
		return;

	// Receive the data
	ssize_t bytes_received =
		recv(conn->sockfd, conn->recv_buffer + conn->recv_len,
			 BUFSIZ - conn->recv_len, 0);

	// If the receive failed, try again
	if (bytes_received < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			conn->state = STATE_RECEIVING_DATA;
		} else {
			perror("recv");
			conn->state = STATE_CONNECTION_CLOSED;
		}
		// Else if no data was received, close the connection
	} else if (bytes_received == 0) {
		conn->state = STATE_CONNECTION_CLOSED;
		// Else if the data was received
	} else {
		// Increment the receive length
		conn->recv_len += bytes_received;
		// If the request is complete, change the state to request received
		if (conn->recv_len >= BUFSIZ || is_request_complete(conn))
			conn->state = STATE_REQUEST_RECEIVED;
		// Else continue receiving data
		else
			conn->state = STATE_RECEIVING_DATA;
	}
}

// Function to handle the input
void handle_input(struct connection *conn)
{
	if (!conn)
		return;

	switch (conn->state) {
	// If the state is initial, then try to receive data
	case STATE_INITIAL:
		conn->state = STATE_RECEIVING_DATA;
		break;
	// Async ongoing for reading dynamic data
	case STATE_ASYNC_ONGOING:
	{
		uint64_t res;
		// Read the eventfd
		int read_res = read(conn->eventfd, &res, sizeof(res));

		if (read_res < 0)
			break;

		// If async reading is complete, send the data
		if (res > 0) {
			// Continue the async io
			connection_continue_async_io(conn);
			// If the all dynamic data was sent, complete the async io
			if (conn->file_pos == conn->file_size)
				connection_complete_async_io(conn);
			// If the chunk of data was read, update the epoll for the sending
			if (conn->state == STATE_SENDING_DATA)
				w_epoll_update_ptr_out(epollfd, conn->eventfd, conn);
			// Else if async io is still ongoing, update the epoll for the reading
			else if (conn->state == STATE_ASYNC_ONGOING)
				w_epoll_update_ptr_in(epollfd, conn->eventfd, conn);
		}
		break;
	}
	// If the state is receiving data, then call the receive data function
	case STATE_RECEIVING_DATA:
		receive_data(conn);
		// If still receiving data or the connection is closed, break
		if (conn->state == STATE_RECEIVING_DATA ||
			conn->state == STATE_CONNECTION_CLOSED)
			break;

		// If cannot parse the header, change the state to sending 404
		if (parse_header(conn) == -1) {
			conn->state = STATE_SENDING_404;
			// Else parse the header
		} else {
			// Get the type of the resource
			conn->res_type = connection_get_resource_type(conn);
			// If the resource is static or dynamic, try to open the file
			if (conn->res_type == RESOURCE_TYPE_STATIC ||
				conn->res_type == RESOURCE_TYPE_DYNAMIC) {
				// If the file cannot be opened, change the state to sending 404
				if (connection_open_file(conn) != 0)
					conn->state = STATE_SENDING_404;
			} else {
				conn->state = STATE_SENDING_404;
			}
		}
		break;

	default:
		// If the state is not valid, change the state to connection closed
		conn->state = STATE_CONNECTION_CLOSED;
		break;
	}
}

void handle_output(struct connection *conn)
{
	if (!conn)
		return;

	switch (conn->state) {
	// If the state is request received, then prepare the header
	case STATE_REQUEST_RECEIVED:
		connection_prepare_send_reply_header(conn);
		conn->state = STATE_SENDING_HEADER;
		break;
	// If the state is sending header, then send the data
	case STATE_SENDING_HEADER:
		// If the header cannot be sent, change the state to connection closed
		if (connection_send_data(conn) == -1) {
			conn->state = STATE_CONNECTION_CLOSED;
			// Else if the header was sent, then begin sending the data
		} else if (conn->send_len == 0) {
			// If the resource is static, change the state to sending data
			if (conn->res_type == RESOURCE_TYPE_STATIC) {
				conn->state = STATE_SENDING_DATA;
				// Else if the resource is dynamic, start the async io
			} else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
				connection_start_async_io(conn);
				conn->state = STATE_ASYNC_ONGOING;
			}
		}
		break;
	// Sending data
	case STATE_SENDING_DATA:
		// If the resource is static, call the static function
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			// If all the data was sent, change the state to connection closed
			if (connection_send_static(conn) == STATE_DATA_SENT)
				conn->state = STATE_CONNECTION_CLOSED;
			// Else if the resource is dynamic, call the dynamic function
		} else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
			// If the send failed, change the state to connection closed
			if (connection_send_dynamic(conn) == -1)
				conn->state = STATE_CONNECTION_CLOSED;
			// If all the data was sent, change the state to connection closed
			if (conn->state == STATE_DATA_SENT)
				conn->state = STATE_CONNECTION_CLOSED;
		}
		break;
	// If the state is sending 404, then prepare the 404 header
	case STATE_SENDING_404:
		// Prepare the 404 header
		connection_prepare_send_404(conn);
		// Send the 404 header
		if (connection_send_data(conn) == -1)
			conn->state = STATE_CONNECTION_CLOSED;
		else if (conn->send_len == 0)
			conn->state = STATE_CONNECTION_CLOSED;
		break;

	default:
		// If the state is not valid, change the state to connection closed
		conn->state = STATE_CONNECTION_CLOSED;
		break;
	}
}

void update_states(int epollfd, struct connection *conn)
{
	int rc;

	// If the state is sending data or request received or sending 404, update the
	// epoll for the sending
	if (conn->state == STATE_SENDING_DATA ||
		conn->state == STATE_REQUEST_RECEIVED ||
		conn->state == STATE_SENDING_404) {
		rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		// Else if the state is receiving data or initial or async ongoing, update
		// the epoll for the reading
	} else if (conn->state == STATE_RECEIVING_DATA ||
			 conn->state == STATE_INITIAL ||
			 conn->state == STATE_ASYNC_ONGOING) {
		rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
	}

	if (rc < 0) {
		perror("w_epoll_update_ptr");
		conn->state = STATE_CONNECTION_CLOSED;
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	int rc;

	if (!conn)
		return;

	// If is input event, call the handle input function
	if (event & EPOLLIN)
		handle_input(conn);
	// If is output event, call the handle output function
	if (event & EPOLLOUT)
		handle_output(conn);
	// If the state is connection closed, remove the connection
	if (conn->state == STATE_CONNECTION_CLOSED) {
		rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		if (rc < 0)
			perror("w_epoll_remove_ptr");
		connection_remove(conn);
		return;
	}
	// Update the epoll
	update_states(epollfd, conn);
}

int main(void)
{
	int rc;

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	while (1) {
		struct epoll_event rev;

		/* wait for events */

		rc = w_epoll_wait_infinite(epollfd, &rev);

		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			handle_client(rev.events, rev.data.ptr);
		}
	}

	close(listenfd);
	return 0;
}
