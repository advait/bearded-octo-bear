/**
 * Copyright 2012 Advait Shinde
 * CS118 - Event-based HTTP Proxy
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>

using namespace std;

#define LISTENING_PORT 12345
#define LISTEN_BACKLOG 5

// Gloabls
void acceptConnection();
int server_fd;


/**
 * Print error and exit 1
 */
void error(const char *msg) {
  perror(msg);
  exit(1);
}


/**
 * Sets the socket fd to non-blocking mode where all calls to read and write
 * return immediately
 */
int setNonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) flags = 0;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


int main (int argc, char *argv[]) {
  // Setup listening socket
  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (!server_fd) {
    error("Unable to create listening socket");
  }
  
  // Bind listening socket to address and port
  sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(LISTENING_PORT);
  if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
    error("Unable to bind listening socket");
  }
  if (listen(server_fd, LISTEN_BACKLOG) < 0) {
    error("Unable to listen to socket fd");
  }
  setNonblocking(server_fd);  // Force server into nonblocking mode
  printf("Listening on port %d\n", LISTENING_PORT);
  
  // Setup polling on server socket
  int nfds = 0;
  struct pollfd server_poll_fd;
  server_poll_fd.fd = server_fd;
  server_poll_fd.events = 0 | POLLIN;  // Unblock when socket is readable
  nfds++;
  int rv = poll(&server_poll_fd, 1, 10000);
  
  if (rv < 0) {
    error("Unable to poll on socket");
  } else if (rv == 0) {
    error("Timeout ocurred");
  } else {
    printf("We can accept from server fd!\n");
    acceptConnection();
  }
  return 0;
}

void acceptConnection() {
  // Accept connection
  sockaddr_in client_addr;
  int client_fd;
  socklen_t client_addr_len = sizeof(client_addr);
  client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
  if (client_fd < 0) {
    error("Unable to accept connection");
  }
  
  // Read request
  char buffer[256];
  int n = read(client_fd, buffer, 256);
  if (n < 0) {
    error("Unable to read from socket");
  }
  
  // Write response
  if (write(client_fd, "Got your message bro", 20) < 0) {
    error("Unable to write to socket");
  }
  close(client_fd);  
}

