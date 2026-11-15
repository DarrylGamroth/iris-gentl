#define _GNU_SOURCE
#include "allocations.h"

#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);

static _Atomic int active;
static _Atomic uint64_t calls[2], bytes[2];
static _Thread_local int paused, inside;

void allocation_begin(void)
{
    for (int i = 0; i < 2; ++i) {
        atomic_store(&calls[i], 0);
        atomic_store(&bytes[i], 0);
    }
    atomic_store(&active, 1);
}
void allocation_pause_thread(int pause) { paused = pause; }
allocation_counts allocation_end(void)
{
    atomic_store(&active, 0);
    return (allocation_counts){atomic_load(&calls[0]), atomic_load(&bytes[0]),
                               atomic_load(&calls[1]), atomic_load(&bytes[1])};
}
static int is_consumer(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    static const char library[] = "libiris.so";
    return strcmp(name, library) == 0 ||
           (strncmp(name, library, sizeof library - 1) == 0 && name[sizeof library - 1] == '.');
}
static void count(size_t size, void *caller)
{
    if (!atomic_load_explicit(&active, memory_order_relaxed) || inside || paused) return;
    inside = 1;
    Dl_info info;
    int consumer = 0;
    if (dladdr(caller, &info)) {
        if (is_consumer(info.dli_fname)) {
            consumer = 1;
        } else if (strstr(info.dli_fname, "/libstdc++.so") || strstr(info.dli_fname, "/libgcc_s.so")) {
            void *stack[12];
            const int depth = backtrace(stack, sizeof(stack) / sizeof(*stack));
            for (int i = 2; i < depth; ++i) {
                if (!dladdr(stack[i], &info)) continue;
                if (strstr(info.dli_fname, "/libstdc++.so") || strstr(info.dli_fname, "/libgcc_s.so"))
                    continue;
                consumer = is_consumer(info.dli_fname);
                break;
            }
        }
    }
    atomic_fetch_add_explicit(&calls[!consumer], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&bytes[!consumer], size, memory_order_relaxed);
    inside = 0;
}
#define COUNT(size) count(size, __builtin_return_address(0))
void *malloc(size_t size) { COUNT(size); return __libc_malloc(size); }
void *calloc(size_t n, size_t size) { COUNT(n * size); return __libc_calloc(n, size); }
void *realloc(void *p, size_t size) { COUNT(size); return __libc_realloc(p, size); }
void *memalign(size_t a, size_t size) { COUNT(size); return __libc_memalign(a, size); }
void *aligned_alloc(size_t a, size_t size) { COUNT(size); return __libc_memalign(a, size); }
int posix_memalign(void **p, size_t a, size_t size)
{
    if (a < sizeof(void *) || (a & (a - 1))) return EINVAL;
    COUNT(size);
    void *result = __libc_memalign(a, size);
    if (!result) return ENOMEM;
    *p = result;
    return 0;
}
void *valloc(size_t size) { COUNT(size); return __libc_memalign((size_t)sysconf(_SC_PAGESIZE), size); }
