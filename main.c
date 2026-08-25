#include <stdio.h>
#include "tun/tun.h"

int main(void) {
    printf("resonance: in-process tests are make check\n");
    int fd;
    
    open_tun_file("res0", &fd);

    return (0);
}
