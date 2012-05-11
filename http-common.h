/**
 * http-common.h
 * Defines common utilities used throughout the project
 */

#ifndef http_common_h
#define http_common_h

#include <stdio.h>
#include <stdlib.h>

// Print error and exit 1
void error(const char *msg) {
  perror(msg);
  exit(1);
}

#endif
