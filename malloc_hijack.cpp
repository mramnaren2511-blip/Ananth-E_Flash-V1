// ============================================================================
// ANANTH E-FLASH V2.0 — GLOBAL C/C++/POSIX ALLOCATOR OVERRIDE
// Role: Zero-Recursion TLS | MMU Page-Safe Guard | LD_PRELOAD Hooks
// Author: Mane Ram Naren (Ananth E Tech)
// 
// DEVELOPER'S NOTE:
// Hello! This file handles the dangerous job of intercepting all OS memory 
// calls and routing them to the Ananth Engine. While I've done my best to 
// make it completely safe and zero-overhead, intercepting system calls is tricky. 
// 
// If you are a kernel developer, OS researcher, or senior C++ engineer, I 
// humbly ask you to review this logic. Please tear it apart, find my mistakes, 
// and help me fix them. Any pull requests to improve the safety or speed of 
// these hooks are highly appreciated!
//
// LICENSE: ANANTH E-TECH FAIR-USE LICENSE
// - FREE for individuals & startups (under $2M revenue / $2M funding).
// - Commercial license required for Enterprises (HFTs, Cloud Giants, etc.).
// See LICENSE file for full terms and conditions.
// ============================================================================

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

// Silence false-positive GCC -O3 warning when inspecting (ptr - 16) BlockHeader
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Warray-bounds"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cerrno>
#include <limits>
#include <new>
#include <atomic>
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>

#include "flash.h"

#define FLASH_EXPORT   __attribute__((visibility("default"), used))
#define FLASH_NOINLINE __attribute__((noinline))

// Initialize Ananth E-Flash immediately upon library/binary load
__attribute__((constructor(101)))
static void eflash_module_autoload() noexcept {
    init_eflash_boot_sequence();
}

// ============================================================================
// 1. BOOTSTRAP STATIC ARENA (FOR EARLY DYNAMIC LINKER / TLS ALLOCATIONS)
// ============================================================================
struct alignas(16) EarlyHeader {
    uint64_t magic;
    uint64_t size;
};
constexpr uint64_t EARLY_MAGIC    = 0xEFA54EFA54EFA540ULL;
constexpr size_t   EARLY_BUF_SIZE = 8 * 1024 * 1024; // 8 MB Demand-Paged Static Bootstrap Pool

alignas(4096) static unsigned char early_alloc_buf[EARLY_BUF_SIZE];
static std::atomic<size_t> early_alloc_offset{0};

// State: 0 = Active E-Flash, 1 = In TLS Recursion
static __thread uint8_t in_tls_alloc __attribute__((tls_model("initial-exec"))) = 0;

FLASH_ALWAYS_INLINE bool is_early_alloc(const void* ptr) noexcept {
    uintptr_t p   = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t beg = reinterpret_cast<uintptr_t>(early_alloc_buf);
    return (p >= beg + sizeof(EarlyHeader)) && (p < beg + EARLY_BUF_SIZE);
}

FLASH_ALWAYS_INLINE size_t get_early_usable_size(const void* ptr) noexcept {
    const EarlyHeader* hdr = reinterpret_cast<const EarlyHeader*>(ptr) - 1;
    return (hdr->magic == EARLY_MAGIC) ? static_cast<size_t>(hdr->size) : 64;
}

FLASH_NOINLINE void* get_early_alloc(size_t size, size_t alignment = 16) noexcept {
    if (alignment < 16) alignment = 16;
    size_t safe_size = (size == 0) ? 16 : size;

    size_t old_off  = early_alloc_offset.load(std::memory_order_relaxed);
    size_t user_off = 0;
    size_t next_off = 0;

    do {
        size_t base_addr    = reinterpret_cast<uintptr_t>(early_alloc_buf) + old_off + sizeof(EarlyHeader);
        size_t aligned_addr = (base_addr + alignment - 1) & ~(alignment - 1);
        user_off = aligned_addr - reinterpret_cast<uintptr_t>(early_alloc_buf);
        next_off = user_off + safe_size;
        if (__builtin_expect(next_off > EARLY_BUF_SIZE, 0)) {
            // Safe OS mmap fallback if 8MB bootstrap pool is ever exceeded
            size_t total_req = safe_size + alignment + sizeof(EarlyHeader);
            void* raw = os_page_alloc(total_req);
            if (!raw) return nullptr;
            uintptr_t raw_base = reinterpret_cast<uintptr_t>(raw);
            uintptr_t u_addr   = (raw_base + sizeof(EarlyHeader) + alignment - 1) & ~(alignment - 1);
            EarlyHeader* eh    = reinterpret_cast<EarlyHeader*>(u_addr) - 1;
            eh->magic = EARLY_MAGIC;
            eh->size  = safe_size;
            return reinterpret_cast<void*>(u_addr);
        }
    } while (!early_alloc_offset.compare_exchange_weak(
        old_off, next_off, std::memory_order_acq_rel, std::memory_order_relaxed));

    void* user_ptr   = &early_alloc_buf[user_off];
    EarlyHeader* hdr = reinterpret_cast<EarlyHeader*>(user_ptr) - 1;
    hdr->magic = EARLY_MAGIC;
    hdr->size  = safe_size;
    return user_ptr;
}

// ============================================================================
// 2. MMU-SAFE POINTER OWNERSHIP & SELF-HEALING PAGE-BOUNDARY REGISTRY
// ============================================================================
static std::atomic<uintptr_t> eflash_min{UINTPTR_MAX};
static std::atomic<uintptr_t> eflash_max{0};

// Demand-paged 256K-slot registry used exclusively for explicit >=4KB aligned allocations
constexpr size_t PAGE_REG_SLOTS = 262144;
static std::atomic<uintptr_t> page_aligned_registry[PAGE_REG_SLOTS];

FLASH_ALWAYS_INLINE void register_page_aligned_ptr(uintptr_t p) noexcept {
    size_t idx = ((p >> 12) * 11400714819323198485ULL) & (PAGE_REG_SLOTS - 1);
    for (size_t step = 0; step < PAGE_REG_SLOTS; ++step) {
        size_t slot   = (idx + step) & (PAGE_REG_SLOTS - 1);
        uintptr_t cur = page_aligned_registry[slot].load(std::memory_order_relaxed);
        if (cur == p) return;
        if (cur == 0 || cur == 1) {
            if (page_aligned_registry[slot].compare_exchange_strong(
                    cur, p, std::memory_order_release, std::memory_order_relaxed)) {
                return;
            }
        }
    }
}

FLASH_ALWAYS_INLINE bool is_registered_page_aligned_ptr(uintptr_t p) noexcept {
    size_t idx = ((p >> 12) * 11400714819323198485ULL) & (PAGE_REG_SLOTS - 1);
    for (size_t step = 0; step < PAGE_REG_SLOTS; ++step) {
        size_t slot   = (idx + step) & (PAGE_REG_SLOTS - 1);
        uintptr_t cur = page_aligned_registry[slot].load(std::memory_order_acquire);
        if (cur == p) return true;
        if (cur == 0) return false;
    }
    return false;
}

FLASH_ALWAYS_INLINE void unregister_page_aligned_ptr(uintptr_t p) noexcept {
    size_t idx = ((p >> 12) * 11400714819323198485ULL) & (PAGE_REG_SLOTS - 1);
    for (size_t step = 0; step < PAGE_REG_SLOTS; ++step) {
        size_t slot   = (idx + step) & (PAGE_REG_SLOTS - 1);
        uintptr_t cur = page_aligned_registry[slot].load(std::memory_order_acquire);
        if (cur == p) {
            // Self-Healing Deletion: If next slot is 0, reset directly to 0 and clean preceding tombstones
            size_t next_slot = (slot + 1) & (PAGE_REG_SLOTS - 1);
            if (page_aligned_registry[next_slot].load(std::memory_order_acquire) == 0) {
                page_aligned_registry[slot].store(0, std::memory_order_release);
                size_t back = slot;
                for (size_t bstep = 0; bstep < 64; ++bstep) {
                    back = (back - 1) & (PAGE_REG_SLOTS - 1);
                    uintptr_t expected_tomb = 1;
                    if (!page_aligned_registry[back].compare_exchange_strong(
                            expected_tomb, 0, std::memory_order_release, std::memory_order_relaxed)) {
                        break;
                    }
                }
            } else {
                page_aligned_registry[slot].store(1, std::memory_order_release);
            }
            return;
        }
        if (cur == 0) return;
    }
}

FLASH_ALWAYS_INLINE void track_eflash_allocation(void* ptr, size_t size) noexcept {
    uintptr_t p     = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t p_end = p + ((size == 0) ? 16 : size);

    uintptr_t cur_min = eflash_min.load(std::memory_order_relaxed);
    while (p < cur_min && !eflash_min.compare_exchange_weak(cur_min, p, std::memory_order_relaxed)) {}

    uintptr_t cur_max = eflash_max.load(std::memory_order_relaxed);
    while (p_end > cur_max && !eflash_max.compare_exchange_weak(cur_max, p_end, std::memory_order_relaxed)) {}

    if (__builtin_expect((p & (FLASH_PAGE_SIZE - 1)) == 0, 0)) {
        register_page_aligned_ptr(p);
    }
}

FLASH_ALWAYS_INLINE bool is_eflash_managed_pointer(const void* ptr) noexcept {
    if (!ptr) return false;
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);

    if (__builtin_expect((p & 15) != 0, 0)) return false;

    if (__builtin_expect(p < eflash_min.load(std::memory_order_relaxed) ||
                          p > eflash_max.load(std::memory_order_relaxed), 0)) {
        return false;
    }

    if (__builtin_expect((p & (FLASH_PAGE_SIZE - 1)) == 0, 0)) {
        if (!is_registered_page_aligned_ptr(p)) return false;
    }

    const BlockHeader* hdr = reinterpret_cast<const BlockHeader*>(ptr) - 1;
    god_reg_t tag = hdr->god_tag.load(std::memory_order_acquire);
    if (!is_valid_allocated(tag)) return false;

    uintptr_t span_addr = reinterpret_cast<uintptr_t>(hdr->parent_span);
    if (span_addr == 0 || (span_addr & (FLASH_PAGE_SIZE - 1)) != 0) return false;

    uint8_t bin = get_size_class_reg(tag);
    if (bin < FLASH_NUM_BINS) {
        uint64_t expected_offset = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hdr) - span_addr);
        return (get_offset(tag) == expected_offset) && (expected_offset < FLASH_SPAN_SIZE);
    } else if (bin == FLASH_LARGE_BIN_TAG) {
        return (reinterpret_cast<uintptr_t>(hdr) >= span_addr) && (get_offset(tag) > 0);
    }
    return false;
}

// ============================================================================
// 3. NOINLINE CORE INVOCATION WRAPPERS
// ============================================================================
FLASH_NOINLINE void* invoke_eflash_malloc(size_t size) noexcept {
    return eflash_malloc_core(size);
}

FLASH_NOINLINE void* invoke_eflash_aligned_alloc(size_t alignment, size_t size) noexcept {
    return (alignment <= 16 && size <= FLASH_MAX_FAST_ALLOC)
        ? eflash_malloc_core(size)
        : eflash_aligned_alloc_core(alignment, size);
}

FLASH_NOINLINE void invoke_eflash_free(void* ptr) noexcept {
    eflash_free_core(ptr);
}

FLASH_ALWAYS_INLINE void* safe_malloc_wrapper(size_t size) noexcept {
    if (__builtin_expect(in_tls_alloc != 0, 0)) {
        return get_early_alloc(size, 16);
    }
    in_tls_alloc = 1;
    asm volatile("" ::: "memory");
    void* ptr = invoke_eflash_malloc(size);
    asm volatile("" ::: "memory");
    in_tls_alloc = 0;

    if (__builtin_expect(ptr != nullptr, 1)) {
        track_eflash_allocation(ptr, size);
    } else {
        errno = ENOMEM;
    }
    return ptr;
}

FLASH_ALWAYS_INLINE void* safe_aligned_alloc_wrapper(size_t alignment, size_t size) noexcept {
    if (alignment < 16) alignment = 16;

    if ((alignment & (alignment - 1)) != 0) {
        if (alignment > (std::numeric_limits<size_t>::max() >> 1)) {
            errno = ENOMEM;
            return nullptr;
        }
        alignment = 1ULL << (64 - __builtin_clzll(alignment - 1));
    }

    if (__builtin_expect(size > std::numeric_limits<size_t>::max() - alignment - sizeof(BlockHeader) - FLASH_PAGE_SIZE, 0)) {
        errno = ENOMEM;
        return nullptr;
    }

    if (__builtin_expect(in_tls_alloc != 0, 0)) {
        return get_early_alloc(size, alignment);
    }

    in_tls_alloc = 1;
    asm volatile("" ::: "memory");
    void* ptr = invoke_eflash_aligned_alloc(alignment, size);
    asm volatile("" ::: "memory");
    in_tls_alloc = 0;

    if (__builtin_expect(ptr != nullptr, 1)) {
        track_eflash_allocation(ptr, size);
    } else {
        errno = ENOMEM;
    }
    return ptr;
}

FLASH_ALWAYS_INLINE void safe_free_wrapper(void* ptr) noexcept {
    if (!ptr) return;
    if (__builtin_expect(is_early_alloc(ptr), 0)) return;
    if (__builtin_expect(!is_eflash_managed_pointer(ptr), 0)) return;

    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    if (__builtin_expect((p & (FLASH_PAGE_SIZE - 1)) == 0, 0)) {
        unregister_page_aligned_ptr(p);
    }

    if (__builtin_expect(in_tls_alloc != 0, 0)) {
        invoke_eflash_free(ptr);
        return;
    }

    in_tls_alloc = 1;
    asm volatile("" ::: "memory");
    invoke_eflash_free(ptr);
    asm volatile("" ::: "memory");
    in_tls_alloc = 0;
}

FLASH_ALWAYS_INLINE size_t safe_usable_size_wrapper(const void* ptr) noexcept {
    if (!ptr) return 0;
    if (__builtin_expect(is_early_alloc(ptr), 0)) {
        return get_early_usable_size(ptr);
    }
    if (__builtin_expect(!is_eflash_managed_pointer(ptr), 0)) {
        return 0;
    }
    return eflash_usable_size(const_cast<void*>(ptr));
}

// ============================================================================
// 4. STANDARD C, POSIX, GLIBC, JEMALLOC & TCMALLOC EXPORTS
// ============================================================================
extern "C" {

    FLASH_EXPORT void* malloc(size_t size) noexcept {
        return safe_malloc_wrapper(size);
    }

    FLASH_EXPORT void free(void* ptr) noexcept {
        safe_free_wrapper(ptr);
    }

    FLASH_EXPORT void cfree(void* ptr) noexcept {
        safe_free_wrapper(ptr);
    }

    FLASH_EXPORT void* calloc(size_t nmemb, size_t size) noexcept {
        size_t total_size = 0;
        if (__builtin_expect(__builtin_mul_overflow(nmemb, size, &total_size), 0)) {
            errno = ENOMEM;
            return nullptr;
        }

        if (__builtin_expect(in_tls_alloc == 1, 0)) {
            void* ptr = get_early_alloc(total_size, 16);
            if (ptr) std::memset(ptr, 0, get_early_usable_size(ptr));
            return ptr;
        }

        void* ptr = safe_malloc_wrapper(total_size);
        if (__builtin_expect(!ptr, 0)) return nullptr;

        if (__builtin_expect(total_size <= FLASH_MAX_FAST_ALLOC, 1)) {
            size_t clear_bytes = eflash_usable_size(ptr);
            std::memset(ptr, 0, clear_bytes);
        }
        return ptr;
    }

    FLASH_EXPORT void* realloc(void* ptr, size_t new_size) noexcept {
        if (!ptr) return safe_malloc_wrapper(new_size);
        if (new_size == 0) {
            safe_free_wrapper(ptr);
            return nullptr;
        }

        if (__builtin_expect(is_early_alloc(ptr), 0)) {
            size_t old_size = get_early_usable_size(ptr);
            if (new_size <= old_size) return ptr;
            void* new_ptr = safe_malloc_wrapper(new_size);
            if (new_ptr) std::memcpy(new_ptr, ptr, old_size);
            return new_ptr;
        }

        if (__builtin_expect(!is_eflash_managed_pointer(ptr), 0)) {
            void* new_ptr = safe_malloc_wrapper(new_size);
            if (!new_ptr) return nullptr;

            size_t page_rem = FLASH_PAGE_SIZE - (reinterpret_cast<uintptr_t>(ptr) & (FLASH_PAGE_SIZE - 1));
            size_t copy_bytes = (new_size < page_rem) ? new_size : page_rem;
            std::memcpy(new_ptr, ptr, copy_bytes);
            return new_ptr;
        }

        size_t old_size = eflash_usable_size(ptr);
        if (new_size <= old_size && (old_size <= FLASH_MAX_FAST_ALLOC || new_size >= (old_size >> 1))) {
            return ptr;
        }

        void* new_ptr = safe_malloc_wrapper(new_size);
        if (__builtin_expect(!new_ptr, 0)) return nullptr;

        size_t copy_bytes = (old_size < new_size) ? old_size : new_size;
        std::memcpy(new_ptr, ptr, copy_bytes);
        safe_free_wrapper(ptr);
        return new_ptr;
    }

    FLASH_EXPORT void* reallocarray(void* ptr, size_t nmemb, size_t size) noexcept {
        size_t total_size = 0;
        if (__builtin_expect(__builtin_mul_overflow(nmemb, size, &total_size), 0)) {
            errno = ENOMEM;
            return nullptr;
        }
        return realloc(ptr, total_size);
    }

    FLASH_EXPORT int posix_memalign(void** memptr, size_t alignment, size_t size) noexcept {
        if (__builtin_expect(!memptr, 0)) return EINVAL;
        if (__builtin_expect(alignment % sizeof(void*) != 0 || (alignment & (alignment - 1)) != 0 || alignment == 0, 0)) {
            return EINVAL;
        }

        int saved_errno = errno;
        void* ptr = safe_aligned_alloc_wrapper(alignment, size);
        errno = saved_errno;

        if (__builtin_expect(!ptr, 0)) return ENOMEM;
        *memptr = ptr;
        return 0;
    }

    FLASH_EXPORT void* aligned_alloc(size_t alignment, size_t size) noexcept {
        if (__builtin_expect(alignment == 0 || (alignment & (alignment - 1)) != 0, 0)) {
            errno = EINVAL;
            return nullptr;
        }
        return safe_aligned_alloc_wrapper(alignment, size);
    }

    FLASH_EXPORT void* memalign(size_t alignment, size_t size) noexcept {
        if (__builtin_expect(alignment == 0, 0)) return safe_malloc_wrapper(size);
        return safe_aligned_alloc_wrapper(alignment, size);
    }

    FLASH_EXPORT void* valloc(size_t size) noexcept {
        return safe_aligned_alloc_wrapper(FLASH_PAGE_SIZE, size);
    }

    FLASH_EXPORT void* pvalloc(size_t size) noexcept {
        if (__builtin_expect(size > std::numeric_limits<size_t>::max() - FLASH_PAGE_SIZE, 0)) {
            errno = ENOMEM;
            return nullptr;
        }
        size_t rounded = (size + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
        return safe_aligned_alloc_wrapper(FLASH_PAGE_SIZE, rounded);
    }

    FLASH_EXPORT size_t malloc_usable_size(void* ptr) noexcept {
        return safe_usable_size_wrapper(ptr);
    }

    FLASH_EXPORT int mallopt(int, int) noexcept { return 1; }
    FLASH_EXPORT int malloc_trim(size_t) noexcept { return 0; }

    // GLIBC INTERNAL ALIAS HOOKS
    FLASH_EXPORT void*  __libc_malloc(size_t size) noexcept                    { return safe_malloc_wrapper(size); }
    FLASH_EXPORT void   __libc_free(void* ptr) noexcept                        { safe_free_wrapper(ptr); }
    FLASH_EXPORT void   __libc_cfree(void* ptr) noexcept                       { safe_free_wrapper(ptr); }
    FLASH_EXPORT void*  __libc_calloc(size_t n, size_t size) noexcept          { return calloc(n, size); }
    FLASH_EXPORT void*  __libc_realloc(void* p, size_t size) noexcept          { return realloc(p, size); }
    FLASH_EXPORT void*  __libc_memalign(size_t a, size_t s) noexcept           { return memalign(a, s); }
    FLASH_EXPORT void*  __libc_valloc(size_t size) noexcept                    { return valloc(size); }
    FLASH_EXPORT void*  __libc_pvalloc(size_t size) noexcept                   { return pvalloc(size); }
    FLASH_EXPORT int    __posix_memalign(void** r, size_t a, size_t s) noexcept { return posix_memalign(r, a, s); }

    // JEMALLOC EXTENDED API HOOKS
    FLASH_EXPORT void* mallocx(size_t size, int flags) noexcept {
        size_t align = (flags & 0x3f) ? (1ULL << (flags & 0x3f)) : 16;
        void* ptr = (align <= 16) ? safe_malloc_wrapper(size) : safe_aligned_alloc_wrapper(align, size);
        if (ptr && (flags & 0x40)) {
            std::memset(ptr, 0, safe_usable_size_wrapper(ptr));
        }
        return ptr;
    }

    FLASH_EXPORT void* rallocx(void* ptr, size_t size, int flags) noexcept {
        size_t old_sz = safe_usable_size_wrapper(ptr);
        void* new_ptr = realloc(ptr, size);
        if (new_ptr && (flags & 0x40)) {
            size_t new_sz = safe_usable_size_wrapper(new_ptr);
            if (new_sz > old_sz) std::memset(static_cast<char*>(new_ptr) + old_sz, 0, new_sz - old_sz);
        }
        return new_ptr;
    }

    FLASH_EXPORT size_t xallocx(void* ptr, size_t, size_t, int) noexcept { return safe_usable_size_wrapper(ptr); }
    FLASH_EXPORT size_t sallocx(const void* ptr, int) noexcept           { return safe_usable_size_wrapper(ptr); }
    FLASH_EXPORT void   dallocx(void* ptr, int) noexcept                 { safe_free_wrapper(ptr); }
    FLASH_EXPORT void   sdallocx(void* ptr, size_t, int) noexcept        { safe_free_wrapper(ptr); }

    FLASH_EXPORT size_t nallocx(size_t size, int) noexcept {
        if (size == 0) return 0;
        if (size <= FLASH_MAX_FAST_ALLOC) return class_to_size(get_size_class(size));
        return (size + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    }

    FLASH_EXPORT int mallctl(const char*, void* oldp, size_t* oldlenp, void*, size_t) noexcept {
        if (oldp && oldlenp && *oldlenp > 0) std::memset(oldp, 0, *oldlenp);
        return 0;
    }

    FLASH_EXPORT int mallctlnametomib(const char*, size_t* mibp, size_t* miblenp) noexcept {
        if (mibp && miblenp && *miblenp > 0) std::memset(mibp, 0, (*miblenp) * sizeof(size_t));
        return 0;
    }

    FLASH_EXPORT int mallctlbymib(const size_t*, size_t, void* oldp, size_t* oldlenp, void*, size_t) noexcept {
        if (oldp && oldlenp && *oldlenp > 0) std::memset(oldp, 0, *oldlenp);
        return 0;
    }

    FLASH_EXPORT void malloc_stats_print(void (*)(void*, const char*), void*, const char*) noexcept {}

    // Prefixed je_* hooks
    FLASH_EXPORT void*  je_malloc(size_t size) noexcept                                  { return safe_malloc_wrapper(size); }
    FLASH_EXPORT void   je_free(void* ptr) noexcept                                      { safe_free_wrapper(ptr); }
    FLASH_EXPORT void*  je_calloc(size_t n, size_t s) noexcept                           { return calloc(n, s); }
    FLASH_EXPORT void*  je_realloc(void* p, size_t s) noexcept                           { return realloc(p, s); }
    FLASH_EXPORT void*  je_aligned_alloc(size_t a, size_t s) noexcept                    { return aligned_alloc(a, s); }
    FLASH_EXPORT int    je_posix_memalign(void** r, size_t a, size_t s) noexcept         { return posix_memalign(r, a, s); }
    FLASH_EXPORT void*  je_memalign(size_t a, size_t s) noexcept                         { return memalign(a, s); }
    FLASH_EXPORT void*  je_valloc(size_t s) noexcept                                     { return valloc(s); }
    FLASH_EXPORT size_t je_malloc_usable_size(void* ptr) noexcept                        { return safe_usable_size_wrapper(ptr); }
    FLASH_EXPORT void*  je_mallocx(size_t s, int f) noexcept                             { return mallocx(s, f); }
    FLASH_EXPORT void*  je_rallocx(void* p, size_t s, int f) noexcept                    { return rallocx(p, s, f); }
    FLASH_EXPORT size_t je_xallocx(void* p, size_t s, size_t e, int f) noexcept          { return xallocx(p, s, e, f); }
    FLASH_EXPORT size_t je_sallocx(const void* p, int f) noexcept                        { return sallocx(p, f); }
    FLASH_EXPORT void   je_dallocx(void* p, int f) noexcept                              { dallocx(p, f); }
    FLASH_EXPORT void   je_sdallocx(void* p, size_t s, int f) noexcept                   { sdallocx(p, s, f); }
    FLASH_EXPORT size_t je_nallocx(size_t s, int f) noexcept                             { return nallocx(s, f); }
    FLASH_EXPORT int    je_mallctl(const char* n, void* o, size_t* ol, void* nw, size_t nl) noexcept { return mallctl(n, o, ol, nw, nl); }
    FLASH_EXPORT int    je_mallctlnametomib(const char* n, size_t* m, size_t* ml) noexcept            { return mallctlnametomib(n, m, ml); }
    FLASH_EXPORT int    je_mallctlbymib(const size_t* m, size_t ml, void* o, size_t* ol, void* nw, size_t nl) noexcept { return mallctlbymib(m, ml, o, ol, nw, nl); }
    FLASH_EXPORT void   je_malloc_stats_print(void (*w)(void*, const char*), void* c, const char* o) noexcept { malloc_stats_print(w, c, o); }

    // Prefixed tc_* hooks
    FLASH_EXPORT void*  tc_malloc(size_t size) noexcept                                  { return safe_malloc_wrapper(size); }
    FLASH_EXPORT void   tc_free(void* ptr) noexcept                                      { safe_free_wrapper(ptr); }
    FLASH_EXPORT void   tc_cfree(void* ptr) noexcept                                     { safe_free_wrapper(ptr); }
    FLASH_EXPORT void*  tc_calloc(size_t n, size_t s) noexcept                           { return calloc(n, s); }
    FLASH_EXPORT void*  tc_realloc(void* p, size_t s) noexcept                           { return realloc(p, s); }
    FLASH_EXPORT void*  tc_memalign(size_t a, size_t s) noexcept                         { return memalign(a, s); }
    FLASH_EXPORT int    tc_posix_memalign(void** r, size_t a, size_t s) noexcept         { return posix_memalign(r, a, s); }
    FLASH_EXPORT void*  tc_valloc(size_t s) noexcept                                     { return valloc(s); }
    FLASH_EXPORT void*  tc_pvalloc(size_t s) noexcept                                    { return pvalloc(s); }
    FLASH_EXPORT size_t tc_malloc_size(void* ptr) noexcept                               { return safe_usable_size_wrapper(ptr); }

} // extern "C"

// ============================================================================
// 5. STANDARD C++ GLOBAL NEW & DELETE OVERLOADS
// ============================================================================
FLASH_EXPORT void* operator new(size_t size) {
    void* p = safe_malloc_wrapper(size);
    if (__builtin_expect(!p, 0)) throw std::bad_alloc();
    return p;
}

FLASH_EXPORT void* operator new[](size_t size) {
    void* p = safe_malloc_wrapper(size);
    if (__builtin_expect(!p, 0)) throw std::bad_alloc();
    return p;
}

FLASH_EXPORT void* operator new(size_t size, const std::nothrow_t&) noexcept {
    return safe_malloc_wrapper(size);
}

FLASH_EXPORT void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    return safe_malloc_wrapper(size);
}

FLASH_EXPORT void operator delete(void* ptr) noexcept                                { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr) noexcept                              { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete(void* ptr, size_t) noexcept                        { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr, size_t) noexcept                      { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete(void* ptr, const std::nothrow_t&) noexcept         { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr, const std::nothrow_t&) noexcept       { safe_free_wrapper(ptr); }

FLASH_EXPORT void* operator new(size_t size, std::align_val_t al) {
    void* p = safe_aligned_alloc_wrapper(static_cast<size_t>(al), size);
    if (__builtin_expect(!p, 0)) throw std::bad_alloc();
    return p;
}

FLASH_EXPORT void* operator new[](size_t size, std::align_val_t al) {
    void* p = safe_aligned_alloc_wrapper(static_cast<size_t>(al), size);
    if (__builtin_expect(!p, 0)) throw std::bad_alloc();
    return p;
}

FLASH_EXPORT void* operator new(size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    return safe_aligned_alloc_wrapper(static_cast<size_t>(al), size);
}

FLASH_EXPORT void* operator new[](size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    return safe_aligned_alloc_wrapper(static_cast<size_t>(al), size);
}

FLASH_EXPORT void operator delete(void* ptr, std::align_val_t) noexcept                    { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr, std::align_val_t) noexcept                  { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete(void* ptr, size_t, std::align_val_t) noexcept            { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr, size_t, std::align_val_t) noexcept          { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept   { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { safe_free_wrapper(ptr); }

#pragma GCC diagnostic pop