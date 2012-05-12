/**
 * Copyright 2012 Advait Shinde
 * CS118 - Event-based HTTP Proxy
 */

#include "http-common.h"
#include "http-response.h"
#include "http-request.h"
#include <boost/crc.hpp>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <errno.h>
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

// Forward Declarations
inline void eventLoop(int server_fd);
void acceptConnection(int server_fd);
class ProxyState;
struct CacheEntry;

// Globals
vector<ProxyState*> ProxyStates;
map<int, ProxyState*> FDMap;  // Maps fds to their ProxyState handlers
map<int, CacheEntry> HTTPCache;


// Represents an entry in the cache
struct CacheEntry {
  char* response;
  size_t len;
};

// Generates an HttpResponse cstring with the indicated status code
// and message and palces it in buf. 
// It is your responsibility to free this string!
int generateResponse(const string &status_code, const string &status_message, char** buf) {
  HttpResponse r;
  r.SetVersion("1.0");
  r.SetStatusCode(status_code);
  r.SetStatusMsg(status_message);
  r.ModifyHeader("Connection", "close");
  int len = r.GetTotalLength();
  char* out = (char*)malloc(len);
  if (!out) error("Out of memory!");
  r.FormatResponse(out);
  *buf = out;
  return len;
}

// Returns the crc32 checksum of the input string
int crc32(const char* s, size_t len) {
  boost::crc_32_type result;
  result.process_bytes(s, len);
  return result.checksum();
}

enum ProxyStateStates {
  STATE_DEFAULT = -1,       // Newly accepted connection
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
    this->m_request_checksum = 0;
  }
  
  ~ProxyState() {
    // Free our unfreed strings!
    if (m_client_in) {
      free(m_client_in);
      m_client_in = NULL;
    }
    if (m_client_out) {
      m_client_out = NULL;
    }    
    if (m_upstream_in) {
      free(m_upstream_in);
      m_upstream_in = NULL;
    }
    if (m_upstream_out) {
      free(m_upstream_out);
      m_upstream_out = NULL;
    }
    // Close our unclosed sockets
    if (m_client_fd > 0) {
      close(m_client_fd);
      m_client_fd = -1;
    }
    if (m_upstream_fd > 0) {
      close(m_upstream_fd);
      m_upstream_fd = -1;
    }
  }
  
  // Returns the client file descriptor
  int getClientFd() {
    return this->m_client_fd;
  }
  
  // Sets the client file descriptor
  void setClientFd(int client_fd) {
    this->m_client_fd = client_fd;
    this->client_addr = client_addr;
  }
  
  // Returns the upstream file descriptor
  int getUpstreamFd() {
    return this->m_upstream_fd;
  }
  
  // Returns the current state of the proxy 
  const int getState() {
    return this->m_state;
  }
  
  // Advances the current state of the proxy
  // This is the main method
  void advanceState() {
    if (this->m_state == STATE_DEFAULT) {
      FDMap[this->m_client_fd] = this;  // Map client socket to this ProxyState
      this->m_state = STATE_CLIENT_READ;
      
    } else if (this->m_state == STATE_CLIENT_READ) {
      // Read everything from socket
      recvAll(m_client_fd, &m_client_in, &m_client_in_read, &m_client_in_size);
      
      // Get crc32 checksum
      m_request_checksum = crc32(m_client_in, m_client_in_read);
      map<int, CacheEntry>::iterator it = HTTPCache.find(m_request_checksum);
      if (it != HTTPCache.end()) {
        // We found our response in our cache! send it!
        printf("Cache hit on client fd %d\n", m_client_fd);
        printf("Cache length: %d\n", (int)it->second.len);
        m_client_out = (char*)malloc(it->second.len);
        if (!m_client_out) error("Out of memory!");
        memcpy(m_client_out, it->second.response, it->second.len);
        m_client_out_written = 0;
        m_client_out_size = it->second.len;
        m_state = STATE_CLIENT_WRITE;
        printf("Tebow %d %d/%d\n", m_client_fd, m_client_out_written, m_client_out_size);
        return;
      }
      
      // Done reading. Parse http request
      HttpRequest req_in;
      try {
        req_in.ParseRequest(m_client_in, m_client_in_read);
      } catch (ParseException e) {
        // Send 400 Bad Request
        m_client_out_size = generateResponse("400", "Bad Request", &m_client_out);
        m_state = STATE_CLIENT_WRITE;
        return;
      }
      
      // Only process GET and HTTP 1.0
      if (req_in.GetMethod() == HttpRequest::UNSUPPORTED) {
        // Send 501 Not Implemented
        m_client_out_size = generateResponse("501", "Not Implemented", &m_client_out);
        m_state = STATE_CLIENT_WRITE;
        return;
      }
      if (req_in.GetVersion() != "1.0") {
        // Send 505 HTTP Version Not Implemented
        m_client_out_size = generateResponse("505", "HTTP Version Not Supported", &m_client_out);
        m_state = STATE_CLIENT_WRITE;
        return;
      }
      
      printf("New request: %s:%d%s on fd %d\n", req_in.GetHost().c_str(), 
             req_in.GetPort(), req_in.GetPath().c_str(), m_client_fd);
      
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
        // Send 502 Bad Gateway
        m_client_out_size = generateResponse("502", "Bad Gateway", &m_client_out);
        m_state = STATE_CLIENT_WRITE;
        return;
      }
      connect(m_upstream_fd, res->ai_addr, res->ai_addrlen);
      // TODO: Free the thing from getaddrinfo
      
      // Map upstream socket fd to this ProxyState
      FDMap[m_upstream_fd] = this;
      
      // Advance state
      m_state = STATE_UPSTREAM_WRITE;
      
    } else if (this->m_state == STATE_UPSTREAM_WRITE) {
      // Write to upstream socket
      int n_remaining = m_upstream_out_size - m_upstream_out_written;
      int n_written = write(m_upstream_fd, m_upstream_out+m_upstream_out_written, n_remaining);
      if (n_written < 0) {
        error("Error writing to upstream socket");
      }
      m_upstream_out_written += n_written;
      printf("Wrote %d bytes to upstream fd %d\n", n_written, m_upstream_fd);
      if (n_written < n_remaining) {
        // Kernel rejected some of our write.
        // We need to wait resend the remaining bytes
        return;
      }
      
      // Free upstream buffer
      free(m_upstream_out);
      m_upstream_out = NULL;
      m_upstream_out_size = 0;
      m_upstream_out_written = 0;
      
      // Advance state
      m_state = STATE_UPSTREAM_READ;
      
    } else if (this->m_state == STATE_UPSTREAM_READ) {
      // Read from upstream socket
      recvAll(m_upstream_fd, &m_upstream_in, &m_upstream_in_read, &m_upstream_in_size);
      
      // Close upstream socket
      close(m_upstream_fd);
      
      // Remove upstream socket fd from map
      FDMap.erase(m_upstream_fd);
      m_upstream_fd = -1;
      
      // Cache the response
      CacheEntry ce;
      ce.len = m_upstream_in_read;
      ce.response = (char*)malloc(ce.len);
      printf("Cache length: %d\n", (int)ce.len);
      if (!ce.response) error("Out of memory!");
      memcpy(ce.response, m_upstream_in, ce.len);
      HTTPCache[m_request_checksum] = ce;
      
      // Set the data we need to write
      m_client_out = m_upstream_in;
      m_client_out_written = 0;
      m_client_out_size = m_upstream_in_read;
      printf("Brady %d %d/%d\n", m_client_fd, m_client_out_written, m_client_out_size); 
      m_upstream_in = NULL;
      m_upstream_in_read = 0;
      m_upstream_in_size = 0;
      
      // Advance state
      m_state = STATE_CLIENT_WRITE;
      
    } else if (this->m_state == STATE_CLIENT_WRITE) {
      printf("Welker %d %d/%d\n", m_client_fd, m_client_out_written, m_client_out_size);
      if (m_client_out_written > m_client_out_size) {
        error("How the fuck...");
      }
      int n_remaining = m_client_out_size - m_client_out_written;
      int n_written = write(m_client_fd, m_client_out+m_client_out_written, n_remaining);
      if (n_written < 0) {
        error("Could not write to client socket");
      }
      m_client_out_written += n_written;
      n_remaining = m_client_out_size - m_client_out_written;
      if (n_remaining > 0) {
        return;
      }
      if (m_client_out_written < 100) {
        printf("Data: %s\n", m_client_out);
      }
      printf("Wrote %d bytes to client fd %d\n", m_client_out_written, m_client_fd);
      
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
  // Private methods
  
  // Recieves all the data from sock_fd, creates a new buffer in buf, and populates
  // the parameters buf_used and buf_len
  void recvAll(int sock_fd, char** out_buf, int* out_buf_used, int* out_buf_len) {
    int n_read = 0;
    int buf_len = BUFFER_SIZE;
    int free_space = BUFFER_SIZE;
    char* buf = (char*)malloc(buf_len);
    if (!buf) error("Out of memory!");
    while (true) {
      int this_read = recv(sock_fd, buf+n_read, free_space, 0);
      if (this_read == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        } else {
          error("Error reading from socket");
        }
      } else if (this_read == 0) {
        break;  // Connection closed
      }
      n_read += this_read;
      free_space = buf_len - n_read;
      if (free_space == 0) {
        size_t _new_buf_len = buf_len * 2;
        char* _new_buf = (char*)malloc(_new_buf_len);
        memcpy(_new_buf, buf, n_read);
        free(buf);
        buf = _new_buf;
        buf_len = _new_buf_len;
        free_space = buf_len - n_read;
      }
    }
    *out_buf = buf;
    *out_buf_used = n_read;
    *out_buf_len = buf_len;
  }
  
  // Private members
  int m_state;
  char* m_client_in;          // Input downstream buffer
  int m_client_in_read;       // How many bytes have we already read?
  int m_client_in_size;       // The size of the input buffer
  char* m_client_out;         // Output downstream buffer
  int m_client_out_written;   // How many bytes have we already written?
  int m_client_out_size;      // How many total bytes do we need to write?
  char* m_upstream_in;        // Input upstream buffer
  int m_upstream_in_read;     // How many bytes have we already read?
  int m_upstream_in_size;     // The size of the input buffer
  char* m_upstream_out;       // Output upstream buffer
  int m_upstream_out_written; // How many bytes have we already written?
  int m_upstream_out_size;    // How many total bytes do we need to write?
  int m_client_fd;            // Client file descriptor
  int m_upstream_fd;          // Upstream file descriptor
  int m_request_checksum;     // The checksum of the input request
};


// Sets the socket fd to non-blocking mode where all calls to read and write
// return immediately.
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
  
  // Start event loop!
  while (true) {
    eventLoop(server_fd);
  }
}


// This function runs repeatedly
// It is responsible for determining which sockets to poll on and
// dispatching events to their appropriate ProxyState->advanceState() handler
inline void eventLoop(int server_fd) {
  // Determine which sockets we need to poll on!
  vector<pollfd> poll_fds;
  
  // We always poll on the server socket
  struct pollfd server_poll_fd;
  server_poll_fd.fd = server_fd;
  server_poll_fd.events = 0 | POLLIN;  // Unblock when socket is readable
//  printf("Polling on fd %d (server)\n", server_fd);
  poll_fds.push_back(server_poll_fd);
  
  // Which auxilary sockets do we need to poll on?
  for (vector<ProxyState*>::iterator it = ProxyStates.begin(); it < ProxyStates.end(); it++) {
    ProxyState* ps = *it;
    
    int current_state = ps->getState();
    if (current_state == STATE_CLIENT_READ) {
      // Waiting to read from client socket
      pollfd pfd;
      pfd.fd = ps->getClientFd();
      pfd.events = 0 | POLLIN;
      poll_fds.push_back(pfd);
//      printf("Polling on fd %d (client read)\n", pfd.fd);
    } else if (current_state == STATE_UPSTREAM_WRITE) { 
      // Waiting to write to upstream socket
      pollfd pfd;
      pfd.fd = ps->getUpstreamFd();
      pfd.events = 0 | POLLOUT;
//      printf("Polling on fd %d (upstream write)\n", pfd.fd);
      poll_fds.push_back(pfd);
    } else if (current_state == STATE_UPSTREAM_READ) {
      // Waiting to read from upstream socket
      pollfd pfd;
      pfd.fd = ps->getUpstreamFd();
      pfd.events = 0 | POLLIN;
//      printf("Polling on fd %d (upstream read)\n", pfd.fd);
      poll_fds.push_back(pfd);
    } else if (current_state == STATE_CLIENT_WRITE) {
      // Waiting to write to client socket
      pollfd pfd;
      pfd.fd = ps->getClientFd();
      pfd.events = 0 | POLLOUT;
//      printf("Polling on fd %d (client write)\n", pfd.fd);
      poll_fds.push_back(pfd);
    } else {
      error("Invalid Proxy state!");
    }
    
  }
  
  // Begin polling
  int rv = poll((pollfd*)&poll_fds[0], poll_fds.size(), 3000);
  if (rv < 0) {
    error("Unable to poll on socket");
  } else if (rv > 0) {
    //printf("%d events occurred!\n", rv);
    for (vector<pollfd>::iterator it = poll_fds.begin(); it < poll_fds.end(); it++) {
      if (it->fd == server_fd) {
        // Handle events on the server socket
        if (it->revents & POLLIN) {
          //printf("Accepting connection from server_fd\n");
          acceptConnection(server_fd);
        } else if (it->revents & POLLERR || 
                   it->revents & POLLHUP ||
                   it->revents & POLLNVAL) {
          error("Something went wrong with the server socket");
        }
      } else {
        // Hanlde events on auxilary sockets
        ProxyState* ps = FDMap[it->fd];
        int state = ps->getState();
        
//        STATE_DEFAULT = -1,       // Newly accepted connection
//        STATE_CLIENT_READ = 0,    // Waiting to read initial request
//        STATE_UPSTREAM_WRITE = 1, // Waiting to write upstream request
//        STATE_UPSTREAM_READ = 2,  // Waiting to read upstream request
//        STATE_CLIENT_WRITE = 3,   // Waiting to write to downstream client
        
        if (it->revents & POLLIN) {
          if (state == STATE_CLIENT_READ || state == STATE_UPSTREAM_READ) {
            ps->advanceState();
          }
        } else if (it->revents & POLLOUT) {
          if (state == STATE_CLIENT_WRITE || state == STATE_UPSTREAM_WRITE) {
            ps->advanceState();
          }
        } else if (it->revents & POLLERR ||
                   it->revents & POLLHUP ||
                   it->revents & POLLNVAL) {
          error("Something went wrong with a socket");
        }
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
  s->advanceState();
  ProxyStates.push_back(s);
}

