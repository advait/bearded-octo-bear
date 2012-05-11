/**
 * Copyright 2012 Advait Shinde
 * CS118 - Event-based HTTP Proxy
 */

#include <map>
#include <vector>
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
void acceptConnection(int server_fd);
class ProxyState;

// Globals
vector<ProxyState> ProxyStates;
map<int, ProxyState> FDMap;  // Maps fds to their ProxyState handlers

// Print error and exit 1
void error(const char *msg) {
  perror(msg);
  exit(1);
}

enum ProxyStateStates {
  STATE_DEFAULT = -1, // Newly accepted connection
  STATE_CLIENT_READ = 0,    // Waiting to read initial request
  STATE_UPSTREAM_WRITE = 1, // Waiting to write upstream request
  STATE_UPSTREAM_READ = 2,  // Waiting to read upstream request
  STATE_CLIENT_WRITE = 3,   // Waiting to write to downstream client
};

// Describes the current state of the proxy connection
class ProxyState {
public:
  // Methods
  ProxyState() {
    this->m_state = STATE_DEFAULT;
    this->m_client_in = NULL;
    this->m_client_out = NULL;
    this->m_upstream_in = NULL;
    this->m_upstream_out = NULL;
    this->m_client_fd = -1;
    this->m_upstream_fd = -1;
  }
  
  void setClientFd(int client_fd) {
    this->m_client_fd = client_fd;
    this->client_addr = client_addr;
  }
  
  const int getState() {
    return this->m_state;
  }
  
  void setState(int s) {
    this->m_state = s;
  }
  
  // Public members
  sockaddr_in client_addr;
  
private:
  // Private members
  int m_state;
  char* m_client_in;   // Input downstream buffer
  char* m_client_out;  // Output downstream buffer
  char* m_upstream_in; // Input upstream buffer
  char* m_upstream_out;  // Output upstream buffer
  int m_client_fd;
  int m_upstream_fd;
};


// Sets the socket fd to non-blocking mode where all calls to read and write
// return immediately
int setNonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) flags = 0;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


int main (int argc, char *argv[]) {
  // Setup listening socket
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
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
  
  // Determine which sockets we need to poll on!
  vector<pollfd> poll_fds;
  struct pollfd server_poll_fd;  // We always poll on the server socket
  server_poll_fd.fd = server_fd;
  server_poll_fd.events = 0 | POLLIN;  // Unblock when socket is readable
  poll_fds.push_back(server_poll_fd);
  for (vector<ProxyState>::iterator it = ProxyStates.begin(); it < ProxyStates.end(); it++) {
    // Which auxilary sockets do we need to poll on?
    int current_state = it->getState();
    if (current_state == STATE_CLIENT_READ) {
      // Waiting to read from client socket
      pollfd pfd;
      pfd.events = 0 | POLLIN;  // Waiting for read
      poll_fds.push_back(pfd);
    } else {
      perror("Not implemented yet!");
    }
        
//        DEFAULT,        // Newly accepted connection
//        CLIENT_READ,    // Waiting to read initial request
//        UPSTREAM_WRITE, // Waiting to write upstream request
//        UPSTREAM_READ,  // Waiting to read upstream request
//        CLIENT_WRITE,   // Waiting to write to downstream client
  }
  
  // Begin polling
  int rv = poll((pollfd*)&poll_fds[0], poll_fds.size(), 10000);
  
  if (rv < 0) {
    error("Unable to poll on socket");
  } else if (rv == 0) {
    error("Timeout ocurred");
  } else {
    printf("%d events occurred!\n", rv);
    for (vector<pollfd>::iterator it = poll_fds.begin(); it < poll_fds.end(); it++) {
      if (it->fd == server_fd) {
        if (it->revents & POLLIN) {
          // Handle new incoming connection on server_fd
          printf("Accepting connection from server_fd\n");
          acceptConnection(server_fd);
        } else if (it->revents & POLLERR || 
                   it->revents & POLLHUP ||
                   it->revents & POLLNVAL) {
          error("Something was wrong with the socket connection");
        } else {
          error("No events on the server socket! :)");
        }
      } else {
        // Handle regular fd connection
        error("Not implemented!");
      }
    }
  }
}


// Accepts a connection on server_fd, creates a new ProxyState object
// for it, and adds the new client connection fd to our polling vector
void acceptConnection(int server_fd) {
  // Accept connection
  ProxyState* s = new ProxyState();
  socklen_t client_addr_len = sizeof(s->client_addr);
  int client_fd = accept(server_fd, (struct sockaddr *)&(s->client_addr), &client_addr_len);
  if (client_fd < 0) {
    error("Unable to accept connection");
  }
  s->setClientFd(client_fd);
  s->setState(STATE_CLIENT_READ);
  ProxyStates.push_back(*s);
  
  
//  // Read request
//  char buffer[256];
//  int n = read(client_fd, buffer, 256);
//  if (n < 0) {
//    error("Unable to read from socket");
//  }
//  
//  // Write response
//  if (write(client_fd, "Got your message bro", 20) < 0) {
//    error("Unable to write to socket");
//  }
//  close(client_fd);  
}

