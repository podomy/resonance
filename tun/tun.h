#ifndef TUN_H
#define TUN_H

#include <stdbool.h>

bool open_tun_file(const char* name, int* fd);

#endif
