/**
 * Copyright 2012 Advait Shinde
 * CS118 - Event-based HTTP Proxy
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

using namespace std;

#define LISTENING_PORT 12345
#define LISTEN_BACKLOG 5

void error(const char *msg) {
  perror(msg);
  exit(1);
}

int main (int argc, char *argv[]) {
  // Setup listening socket
  int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (!sock_fd) {
    error("Unable to create listening socket");
  }
  
  // Bind listening socket to address and port
  sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(LISTENING_PORT);
  if (bind(sock_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
    error("Unable to bind listening socket");
  }
  if (listen(sock_fd, LISTEN_BACKLOG) < 0) {
    error("Unable to listen to socket fd");
  }
  printf("Listening on port %d\n", LISTENING_PORT);
  
  // Accept connection
  sockaddr_in client_addr;
  int client_fd;
  socklen_t client_addr_len = sizeof(client_addr);
  client_fd = accept(sock_fd, (struct sockaddr *) &client_addr, &client_addr_len);
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
  
  
  return 0;
}
