// strnlen for programs linked with Zig 0.16's own C library (MinGW targets, and tools Zig builds for
// a Windows host, such as NASM). Zig's version loads the whole `max` bytes in vector blocks, so a
// short string near the end of a page reads past it and crashes, which C does not allow: only the
// bytes up to the terminator may be read. Zig's libc symbols are weak, so this one takes their place.
#include <stddef.h>

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}
