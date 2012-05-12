/**
 * Copyright 2012 Advait Shinde
 * CS118 - HTTP Client
 */

#include "http-request.h"
#include "http-common.h"
#include <boost/regex.hpp>
#include <string>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

using namespace std;

#define SERVER_HOST "localhost"
#define SERVER_PORT 12345
#define BUFFER_SIZE 1024

int main (int argc, char *argv[]) {
  // Parse arguments
  if (argc != 2) {
    printf("Usage: http-get [URL]\n");
    exit(1);
  }
  
  char* full_url = argv[1];
  char* host;
  char* path;
  char* filename;
  int port = 80;
  size_t full_url_len = strlen(full_url);
  
  // strip http://
  if (full_url_len > 7) {
    if (strncasecmp("http://", full_url, 7) == 0) {
      full_url += 7;
      full_url_len -= 7;
    }
  }
  
  // Find first slash
  uint32_t i;
  for (i = 0; i < full_url_len; i++) {
    if (full_url[i] == '/') break;
  }
  host = (char*)malloc(i+1);
  path = (char*)malloc(full_url_len - i + 1);
  if (!host || !path) error("Out of memory!");
  strncpy(host, full_url, i);
  host[i] = '\0';
  strncpy(path, full_url+i, full_url_len - i);
  path[full_url_len-i] = '\0'; 
  // Find port
  for (i = 0; i < strlen(host); i++) {
    if (host[i] == ':') {
      host[i] = '\0';  // Don't include the port in the host
      port = atoi(host+i+1);
      break;
    }
  }  

  // Generate filename
  uint32_t last_slash = 0;
  uint32_t path_len = strlen(path);
  for (i = 0; i < path_len; i++) {
    if (path[i] == '/') last_slash = i;
  }
  if (path_len == 0) {
    path = (char*)malloc(2);
    if (!path) error("Out of memory!");
    path[0] = '/';
    path[1] = '\0';
    path_len = 1;
  } 
  if (last_slash == path_len-1) {
    // Default filename
    filename = (char*)malloc(10 + 1);
    if (!filename) error("Out of memory!");
    strncpy(filename, "index.html", 10);
    filename[10] = '\0';
  } else {
    // Generated filename
    printf("Genereated filename %d %d\n", path_len, last_slash);
    filename = (char*)malloc(path_len - last_slash - 1 + 1);
    strncpy(filename, path+last_slash+1, path_len-last_slash-1);
    filename[path_len-last_slash-1] = '\0';
  }
  
  // Create HTTP Request
  HttpRequest r;
  r.SetHost(host);
  r.SetPort(port);
  r.SetMethod(HttpRequest::GET);
  r.SetPath(path);
  r.SetVersion("1.0");
  r.AddHeader("Connection", "close");
  
  // Generate request buffer
  size_t req_len = r.GetTotalLength();
  char* req_buf = (char*)malloc(req_len);
  if (!req_buf) error("Out of memory!");
  r.FormatRequest(req_buf);
  
  // Lookup our host
  struct hostent *hp;
  if ((hp = gethostbyname(SERVER_HOST)) == 0) {
		error("Could not find localhost");
	}
  
  // Open client socket
  char port_str[10];
  sprintf(port_str, "%d", SERVER_PORT);
  int client_fd = socket(AF_INET, SOCK_STREAM, 0);
  addrinfo addr, *res;
  memset(&addr, 0, sizeof addr);
  addr.ai_family = AF_UNSPEC;
  addr.ai_socktype = SOCK_STREAM;
  addr.ai_protocol = IPPROTO_TCP;
  if (getaddrinfo(SERVER_HOST, port_str, &addr, &res) != 0) {
    error("Could not get localhost");
  }
  while (res->ai_family != AF_INET) {  // Skip IPv6 Addresses
    res = res->ai_next;
  }
  if (connect(client_fd, res->ai_addr, res->ai_addrlen) != 0) {
    error("Could not connect to socket");
  }
      
  // Send request
  size_t n_sent = 0;
  while(n_sent < req_len) {
    int _temp = send(client_fd, req_buf+n_sent, req_len-n_sent, 0);
    if (_temp == -1) error("Could not send data through socket");
    n_sent += _temp;
  }
  
  // Read response
  size_t n_recv = 0;
  size_t out_len = BUFFER_SIZE;
  char* out_buf = (char*)malloc(out_len);
  if (!out_buf) error("Out of memory!");
  while (true) {
    printf("LOOP\n");
    int remaining = out_len - n_recv;
    int _temp = recv(client_fd, out_buf+n_recv, remaining, 0);
    printf("DOOP\n");
    n_recv += _temp;
    if (_temp == -1) { 
      error("Could not recv from socket");
    } else if (_temp == 0) {
      // Done reading
      break;
    } else if (n_recv == out_len) {
      // We need to reallocate a buffer twice the size
      int _new_len = out_len*2;
      char* _new_buf = (char*)malloc(_new_len);
      memcpy(_new_buf, out_buf, out_len);
      free(out_buf);
      out_buf = _new_buf;
      out_len = _new_len;
    }
  }
  printf("%s\n", out_buf);
  
  // Find HTTP data payload (minus headers)
  char* payload = (char*)memmem(out_buf, out_len, "\r\n\r\n", 4);
  if (!payload) {
    error("Invalid HTTP Response");
  }
  payload += 4;
  int payload_offset = payload - out_buf;
  int payload_len = n_recv - payload_offset;
  
  // Write output to file
  FILE *file;
  file = fopen(filename,"w");
  fwrite(payload, 1, payload_len, file);
  fclose(file);
  
  // Free allocated strings
  free(host);
  free(path);
  free(filename);
  
  return 0;
}
