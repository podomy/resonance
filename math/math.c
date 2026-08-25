#include "math.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

uint64_t min(uint64_t count, ...) {
    va_list args;
    uint64_t m;
    size_t i;

    if (count == 0)
        return (0);
    va_start(args, count);
    m = va_arg(args, uint64_t);
    for (i = 1; i < count; i++) {
        uint64_t current_arg = va_arg(args, uint64_t);
        if (current_arg < m)
            m = current_arg;
    }
    va_end(args);
    return (m);
}
