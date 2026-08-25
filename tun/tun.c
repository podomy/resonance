#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include "tun.h"

bool open_tun_file(const char* name, int* fd) {
  if(name == NULL || fd == NULL) {
    return false;
  }

  int table_fd = open("/dev/net/tun", O_RDWR);
  if (table_fd < 0) {
    return false;
  }

  *fd = table_fd;

  return true;
}
