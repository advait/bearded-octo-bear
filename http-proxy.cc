/**
 * Copyright 2012 Advait Shinde
 * CS118 - Event-based HTTP Proxy
 */

#include "http-request.h"
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <fcntl.h>
#include <netdb.h>
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
#define BUFFER_SIZE 1024

// Gloabls
void acceptConnection(int server_fd);
class ProxyState;

// Globals
vector<ProxyState*> ProxyStates;
map<int, ProxyState*> FDMap;  // Maps fds to their ProxyState handlers

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
    this->m_client_in_read = 0;
    this->m_client_in_size = 0;
    this->m_client_out = NULL;
    this->m_upstream_in = NULL;
    this->m_upstream_in_read = 0;
    this->m_upstream_in_size = 0;
    this->m_upstream_out = NULL;
    this->m_upstream_out_written = 0;
    this->m_upstream_out_size = 0;
    this->m_client_fd = -1;
    this->m_upstream_fd = -1;
  }
  
  int getClientFd() {
    return this->m_client_fd;
  }
  
  void setClientFd(int client_fd) {
    this->m_client_fd = client_fd;
    this->client_addr = client_addr;
  }
  
  int getUpstreamFd() {
    return this->m_upstream_fd;
  }
  
  const int getState() {
    return this->m_state;
  }
  
  void advanceState() {
    if (this->m_state == STATE_DEFAULT) {
      FDMap[this->m_client_fd] = this;  // Map client socket to this ProxyState
      this->m_state = STATE_CLIENT_READ;
    } else if (this->m_state == STATE_CLIENT_READ) {
      // Read from client socket
      char* buf;
      size_t buf_size = m_client_in_size == 0 ? BUFFER_SIZE : m_client_in_size * 2;
      buf = (char*)malloc(buf_size);
      if (!buf) error("Out of memory");
      if (m_client_in_size > 0) {
        // We've already began the read
        memcpy(buf, m_client_in, m_client_in_size);
        free(m_client_in);
        m_client_in = NULL;
      }
      int n_free_bytes = buf_size - m_client_in_read;
      int n_read = read(m_client_fd, buf+m_client_in_read, n_free_bytes);
      m_client_in = buf;
      m_client_in_read += n_read;
      m_client_in_size = buf_size;
      if (n_read == 0) {
        error("We shouldn't be reading zero bytes...");
      } else if (n_read == n_free_bytes) {
        // The read filled up our buffer. We need to read more.
        // Remain in this state and return.
        return;
      }
      
      // Done reading. Parse http request
      HttpRequest req_in;
      try {
        req_in.ParseRequest(m_client_in, m_client_in_read);
      } catch (ParseException e) {
        // TODO: Send 400 Bad Request
        error("Invalid HTTP Request. TODO: Send 400");
      }
      
      // Only process GET and HTTP 1.0
      if (req_in.GetMethod() == HttpRequest::UNSUPPORTED) {
        // TODO: Send 501 Not Implemented
        error("Unsupported method. TODO: Send 501");
      }
      if (req_in.GetVersion() != "1.0") {
        // TODO: Send 505 HTTP Version Not Implemented
        error("Unsupported HTTP Version. TODO: Send 505");
      }
      
      // Set connection close header
      req_in.ModifyHeader("Conection", "close");
      
      // Generate output upstream http request (char*)
      m_upstream_out_size = req_in.GetTotalLength();
      m_upstream_out = (char *) malloc(m_upstream_out_size);
      if (m_upstream_out == NULL) error("Out of memory!");
      req_in.FormatRequest(m_upstream_out);
      
      // Get IP of upstream host and connect
      string host = req_in.GetHost();
      int port = req_in.GetPort();
      char port_str[10];
      sprintf(port_str, "%d", port);
      m_upstream_fd = socket(AF_INET, SOCK_STREAM, 0);
      addrinfo addr, *res;
      memset(&addr, 0, sizeof addr);
      addr.ai_family = AF_UNSPEC;
      addr.ai_socktype = SOCK_STREAM;
      if (getaddrinfo(host.c_str(), port_str, &addr, &res) != 0) {
        // TODO: Send 502 Bad Gateway
        error("Could not get upstrea host address");
      }
      connect(m_upstream_fd, res->ai_addr, res->ai_addrlen);
      
      //printf("Generated upstream request:\n\n%s\n\n", m_upstream_out);

      // Map upstream socket fd to this ProxyState
      FDMap[m_upstream_fd] = this;
      
      // Advance state
      m_state = STATE_UPSTREAM_WRITE;
    } else if (this->m_state == STATE_UPSTREAM_WRITE) {
      // Write to upstream socket
      int n_remaining = m_upstream_out_size - m_upstream_out_written;
      int n_written = write(m_upstream_fd, m_upstream_out+m_upstream_out_written, n_remaining);
      m_upstream_out_written += n_written;
      printf("Wrote %d bytes to upstream fd %d\n", n_written, m_upstream_fd);
      if (n_written < n_remaining) {
        // Kernel rejected some of our write.
        // We need to wait resend the remaining bytes
        return;
      }
      
      //printf("Request from client\n\n%s\n", m_upstream_out);
      
      // Free upstream buffer
      free(m_upstream_out);
      m_upstream_out = NULL;
      m_upstream_out_size = 0;
      m_upstream_out_written = 0;
      
      // Advance state
      m_state = STATE_UPSTREAM_READ;
    } else if (this->m_state == STATE_UPSTREAM_READ) {
      // Read from upstream socket
      char* buf;
      size_t buf_size = m_upstream_in_size == 0 ? BUFFER_SIZE : m_upstream_in_size * 2;
      buf = (char*)malloc(buf_size);
      if (!buf) error("Out of memory");
      if (m_upstream_in_size > 0) {
        // We've already began the read
        memcpy(buf, m_upstream_in, m_upstream_in_size);
        free(m_upstream_in);
        m_client_in = NULL;
      }
      int n_free_bytes = buf_size - m_upstream_in_read;
      int n_read = read(m_upstream_fd, buf+m_upstream_in_read, n_free_bytes);
      m_upstream_in = buf;
      m_upstream_in_read += n_read;
      m_upstream_in_size = buf_size;
      if (n_read == 0) {
        // TODO: What if our response is EXACTLY BUFFER_SIZE bytes long?
        // We'll initially fill up our buffer and our second call to read
        // will return zero bytes causing this error. FIX IT!
        error("We shouldn't be reading zero bytes...");
      } else if (n_read == n_free_bytes) {
        // The read filled up our buffer. We need to read more.
        // Remain in this state and return.
        return;
      }
      
      // Close upstream socket
      close(m_upstream_fd);
      
      // Remove upstream socket fd from map
      FDMap.erase(m_upstream_fd);
      m_upstream_fd = -1;
      
      // Set the data we need to write
      m_client_out = m_upstream_in;
      m_client_out_size = m_upstream_in_read;
      m_upstream_in = NULL;
      m_upstream_in_read = 0;
      m_upstream_in_size = 0;
      
      //printf("Response to client:\n\n%s\n", m_client_out);
      
      // Advance state
      m_state = STATE_CLIENT_WRITE;
    } else if (this->m_state == STATE_CLIENT_WRITE) {
      // Write to downstream client
      int n_remaining = m_client_out_size - m_client_out_written;
      int n_written = write(m_client_fd, m_client_out+m_client_out_written, n_remaining);
      m_client_out_written += n_written;
      printf("Wrote %d bytes to client fd %d\n", n_written, m_client_fd);
      if (n_written < n_remaining) {
        // Kernel rejected some of our write.
        // We need to wait resend the remaining bytes
        return;
      }
      
      //printf("Wrote response to client\n\n%s\n", m_client_out);
      
      // Free client out downstream buffer
      free(m_client_out);
      m_client_out = NULL;
      m_client_out_size = 0;
      m_client_out_written = 0;
      
      // Close client socket
      close(m_client_fd);
      
      // Remove client fd from map
      FDMap.erase(m_client_fd);
      m_client_fd = -1;
      
      // Remove this ProxyState from ProxyStates vector
      for (vector<ProxyState*>::iterator it = ProxyStates.begin(); it < ProxyStates.end(); it++) {
        if (*it == this) {
          ProxyStates.erase(it);
          break;
        }
      }
      
      // Free this ProxyState!
      delete this;
    }
  }
  
  // Public members
  sockaddr_in client_addr;
  
private:
  // Private members
  int m_state;
  char* m_client_in;    // Input downstream buffer
  int m_client_in_read; // How many bytes have we already read?
  int m_client_in_size; // The size of the input buffer
  char* m_client_out;   // Output downstream buffer
  int m_client_out_written;  // How many bytes have we already written?
  int m_client_out_size;     // How many total bytes do we need to write?
  char* m_upstream_in;  // Input upstream buffer
  int m_upstream_in_read;  // How many bytes have we already read?
  int m_upstream_in_size;  // The size of the input buffer
  char* m_upstream_out; // Output upstream buffer
  int m_upstream_out_written;  // How many bytes have we already written?
  int m_upstream_out_size;     // How many total bytes do we need to write?
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
  
  while (true) {
    // Determine which sockets we need to poll on!
    vector<pollfd> poll_fds;
    struct pollfd server_poll_fd;  // We always poll on the server socket
    server_poll_fd.fd = server_fd;
    server_poll_fd.events = 0 | POLLIN;  // Unblock when socket is readable
    printf("Polling on fd %d (server)\n", server_fd);
    poll_fds.push_back(server_poll_fd);
    for (vector<ProxyState*>::iterator it = ProxyStates.begin(); it < ProxyStates.end(); it++) {
      ProxyState* ps = *it;
      // Which auxilary sockets do we need to poll on?
      int current_state = ps->getState();
      if (current_state == STATE_CLIENT_READ) {
        // Waiting to read from client socket
        pollfd pfd;
        pfd.fd = ps->getClientFd();
        pfd.events = 0 | POLLIN;
        poll_fds.push_back(pfd);
        printf("Polling on fd %d (client read)\n", pfd.fd);
      } else if (current_state == STATE_UPSTREAM_WRITE) { 
        // Waiting to write to upstream socket
        pollfd pfd;
        pfd.fd = ps->getUpstreamFd();
        pfd.events = 0 | POLLOUT;
        printf("Polling on fd %d (upstream write)\n", pfd.fd);
        poll_fds.push_back(pfd);
      } else if (current_state == STATE_UPSTREAM_READ) {
        pollfd pfd;
        pfd.fd = ps->getUpstreamFd();
        pfd.events = 0 | POLLIN;
        printf("Polling on fd %d (upstream read)\n", pfd.fd);
        poll_fds.push_back(pfd);
      } else if (current_state == STATE_CLIENT_WRITE) {
        pollfd pfd;
        pfd.fd = ps->getClientFd();
        pfd.events = 0 | POLLOUT;
        printf("Polling on fd %d (client write)\n", pfd.fd);
        poll_fds.push_back(pfd);
      } else {
        perror("Invalid Proxy state!");
      }

    }
    
    // Begin polling
    int rv = poll((pollfd*)&poll_fds[0], poll_fds.size(), 3000);
    if (rv < 0) {
      error("Unable to poll on socket");
    } else if (rv == 0) {
      //error("Timeout ocurred");
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
            error("Something went wrong with the server socket");
          } else {
            // No events on the server socket
            //error("No events on the server socket! :)");
          }
        } else {
          if (it->revents & POLLIN || it->revents & POLLOUT) {
            // Handle regular fd connection
            FDMap[it->fd]->advanceState();
          } else if (it->revents & POLLERR ||
                     it->revents & POLLHUP ||
                     it->revents & POLLNVAL) {
            error("Something went wrong with a socket");
          } else {
            // No events on this socket
          }
        }
      }
    }
  }  // end while(true)
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
  s->advanceState();
  ProxyStates.push_back(s);
}


