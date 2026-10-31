// Linux/glibc-only test instrumentation, not linked into the plugin.
// LD_PRELOAD counts allocations only in the thread that calls start/stop.
// The worker thread is deliberately excluded: preparing copies there may allocate.
#include <cerrno>
#include <cstddef>
#include <cstdint>

extern "C" {
void* __libc_malloc (std::size_t);
void* __libc_calloc (std::size_t, std::size_t);
void* __libc_realloc (void*, std::size_t);
void* __libc_memalign (std::size_t, std::size_t);
void __libc_free (void*);
}
static thread_local bool tracking = false;
static thread_local std::uint64_t allocations = 0;
static void count() noexcept { if (tracking) ++allocations; }
extern "C" void orogen_allocation_probe_start() { allocations = 0; tracking = true; }
extern "C" std::uint64_t orogen_allocation_probe_stop() { tracking = false; return allocations; }
extern "C" void* malloc (std::size_t n) noexcept { count(); return __libc_malloc (n); }
extern "C" void* calloc (std::size_t n, std::size_t s) noexcept { count(); return __libc_calloc (n, s); }
extern "C" void* realloc (void* p, std::size_t n) noexcept { if (n != 0) count(); return __libc_realloc (p, n); }
extern "C" void free (void* p) noexcept { __libc_free (p); }
extern "C" void* aligned_alloc (std::size_t a, std::size_t n) noexcept { count(); return __libc_memalign (a, n); }
extern "C" void* memalign (std::size_t a, std::size_t n) noexcept { count(); return __libc_memalign (a, n); }
extern "C" int posix_memalign (void** p, std::size_t a, std::size_t n) noexcept
{
    if (a < sizeof (void*) || (a & (a - 1)) != 0) return EINVAL;
    count(); void* v = __libc_memalign (a, n);
    if (v == nullptr) return ENOMEM;
    *p = v; return 0;
}
