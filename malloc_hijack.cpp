// ============================================================================
// ANANTH E-FLASH V2.0 - GLOBAL C/C++/POSIX ALLOCATOR OVERRIDE
// Role: Zero-Recursion TLS | MMU Page-Safe Guard | LD_PRELOAD Hooks
// Author: Mane Ram Naren (Ananth E Tech)
// Patent Pending: 202641127187
//
// LICENSE: ANANTH E-TECH FAIR-USE LICENSE
// - FREE for individuals & startups (under $2M revenue / $2M funding).
// - Commercial license required for Enterprises (HFTs, Cloud Giants, etc.).
// See LICENSE file for full terms and conditions.
// ============================================================================

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

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
#include <dlfcn.h>

#include "flash.h"

#define FLASH_EXPORT   __attribute__((visibility("default"), used))
#define FLASH_NOINLINE __attribute__((noinline))

// ============================================================================
// 0. NEXT-ALLOCATOR (RTLD_NEXT) BRIDGE FOR FOREIGN POINTER SAFETY
// ============================================================================
using real_free_fn_t        = void (*)(void*);
using real_realloc_fn_t     = void* (*)(void*, size_t);
using real_usable_size_fn_t = size_t (*)(void*);

static std::atomic<real_free_fn_t>        g_real_free{nullptr};
static std::atomic<real_realloc_fn_t>     g_real_realloc{nullptr};
static std::atomic<real_usable_size_fn_t> g_real_usable_size{nullptr};
static std::atomic<bool>                  g_rtld_resolved{false};

// State: 0 = Active E-Flash, 1 = In TLS / Resolver Recursion
static __thread uint8_t in_tls_alloc __attribute__((tls_model("initial-exec"))) = 0;

static void resolve_rtld_next_symbols() noexcept {
    if (g_rtld_resolved.load(std::memory_order_acquire)) return;
    uint8_t prev = in_tls_alloc;
    in_tls_alloc = 1;
    asm volatile("" ::: "memory");

    void* f_free = dlsym(RTLD_NEXT, "free");
    void* f_real = dlsym(RTLD_NEXT, "realloc");
    void* f_usab = dlsym(RTLD_NEXT, "malloc_usable_size");

    if (f_free) g_real_free.store(reinterpret_cast<real_free_fn_t>(f_free), std::memory_order_release);
    if (f_real) g_real_realloc.store(reinterpret_cast<real_realloc_fn_t>(f_real), std::memory_order_release);
    if (f_usab) g_real_usable_size.store(reinterpret_cast<real_usable_size_fn_t>(f_usab), std::memory_order_release);
    g_rtld_resolved.store(true, std::memory_order_release);

    asm volatile("" ::: "memory");
    in_tls_alloc = prev;
}

__attribute__((constructor(101)))
static void eflash_module_autoload() noexcept {
    init_eflash_boot_sequence();
    resolve_rtld_next_symbols();
}

// ============================================================================
// 1. BOOTSTRAP STATIC ARENA + FALLBACK TRACKER (EARLY DYNAMIC LINKER / TLS)
// ============================================================================
struct alignas(16) EarlyHeader {
    uint64_t magic;
    uint64_t size;
};
constexpr uint64_t EARLY_MAGIC         = 0xEFA54EFA54EFA540ULL;
constexpr size_t   EARLY_BUF_SIZE      = 8 * 1024 * 1024; // 8 MB Demand-Paged Static Bootstrap Pool
constexpr size_t   EARLY_FALLBACK_MAX  = 1024;

alignas(4096) static unsigned char early_alloc_buf[EARLY_BUF_SIZE];
static std::atomic<size_t>    early_alloc_offset{0};
static std::atomic<uintptr_t> early_fallback_ptrs[EARLY_FALLBACK_MAX];
static std::atomic<size_t>    early_fallback_count{0};

FLASH_ALWAYS_INLINE void track_early_fallback_ptr(uintptr_t p) noexcept {
    size_t idx = early_fallback_count.fetch_add(1, std::memory_order_acq_rel);
    if (idx < EARLY_FALLBACK_MAX) {
        early_fallback_ptrs[idx].store(p, std::memory_order_release);
    }
}

FLASH_ALWAYS_INLINE bool is_early_fallback_ptr(uintptr_t p) noexcept {
    size_t count = early_fallback_count.load(std::memory_order_acquire);
    if (__builtin_expect(count == 0, 1)) return false;
    if (count > EARLY_FALLBACK_MAX) count = EARLY_FALLBACK_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (early_fallback_ptrs[i].load(std::memory_order_acquire) == p) return true;
    }
    return false;
}

FLASH_ALWAYS_INLINE bool is_early_alloc(const void* ptr) noexcept {
    uintptr_t p   = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t beg = reinterpret_cast<uintptr_t>(early_alloc_buf);
    if ((p >= beg + sizeof(EarlyHeader)) && (p < beg + EARLY_BUF_SIZE)) return true;
    return is_early_fallback_ptr(p);
}

FLASH_ALWAYS_INLINE size_t get_early_usable_size(const void* ptr) noexcept {
    const EarlyHeader* hdr = reinterpret_cast<const EarlyHeader*>(ptr) - 1;
    return (hdr->magic == EARLY_MAGIC) ? static_cast<size_t>(hdr->size) : 64;
}

FLASH_NOINLINE void* get_early_alloc(size_t size, size_t alignment = 16) noexcept {
    if (alignment < 16) alignment = 16;
    if ((alignment & (alignment - 1)) != 0) {
        if (alignment > (std::numeric_limits<size_t>::max() >> 1)) return nullptr;
        alignment = 1ULL << (64 - __builtin_clzll(alignment - 1));
    }
    if (alignment > (std::numeric_limits<size_t>::max() >> 2)) return nullptr;

    size_t safe_size = (size == 0) ? 16 : size;
    if (safe_size > std::numeric_limits<size_t>::max() - alignment - sizeof(EarlyHeader) - FLASH_PAGE_SIZE) {
        return nullptr;
    }

    size_t old_off  = early_alloc_offset.load(std::memory_order_relaxed);
    size_t user_off = 0;
    size_t next_off = 0;

    do {
        if (old_off >= EARLY_BUF_SIZE || safe_size > EARLY_BUF_SIZE) {
            goto fallback_mmap;
        }
        uintptr_t base_addr    = reinterpret_cast<uintptr_t>(early_alloc_buf) + old_off + sizeof(EarlyHeader);
        uintptr_t aligned_addr = (base_addr + alignment - 1) & ~(alignment - 1);
        user_off = static_cast<size_t>(aligned_addr - reinterpret_cast<uintptr_t>(early_alloc_buf));
        if (user_off > EARLY_BUF_SIZE || safe_size > (EARLY_BUF_SIZE - user_off)) {
            goto fallback_mmap;
        }
        next_off = user_off + safe_size;
    } while (!early_alloc_offset.compare_exchange_weak(
        old_off, next_off, std::memory_order_acq_rel, std::memory_order_relaxed));

    {
        void* user_ptr   = &early_alloc_buf[user_off];
        EarlyHeader* hdr = reinterpret_cast<EarlyHeader*>(user_ptr) - 1;
        hdr->magic = EARLY_MAGIC;
        hdr->size  = safe_size;
        return user_ptr;
    }

fallback_mmap:
    size_t total_req = safe_size + alignment + sizeof(EarlyHeader);
    void* raw = os_page_alloc(total_req);
    if (!raw) return nullptr;
    uintptr_t raw_base = reinterpret_cast<uintptr_t>(raw);
    uintptr_t u_addr   = (raw_base + sizeof(EarlyHeader) + alignment - 1) & ~(alignment - 1);
    EarlyHeader* eh    = reinterpret_cast<EarlyHeader*>(u_addr) - 1;
    eh->magic = EARLY_MAGIC;
    eh->size  = safe_size;
    track_early_fallback_ptr(u_addr);
    return reinterpret_cast<void*>(u_addr);
}

// ============================================================================
// 2. MMU-SAFE POINTER OWNERSHIP & LOCK-FREE PAGE-BOUNDARY REGISTRY
// ============================================================================
static std::atomic<uintptr_t> eflash_min{UINTPTR_MAX};
static std::atomic<uintptr_t> eflash_max{0};

// Demand-paged 256K-slot lock-free open-addressing registry for >=4KB aligned pointers.
// State values: 0 = Empty (never used), 1 = Tombstone (deleted, chain preserved), else = Pointer.
// IMPORTANT: Never reset tombstones back to 0 concurrently; doing so breaks concurrent probe chains.
constexpr size_t PAGE_REG_SLOTS = 262144;
static std::atomic<uintptr_t> page_aligned_registry[PAGE_REG_SLOTS];

FLASH_ALWAYS_INLINE bool register_page_aligned_ptr(uintptr_t p) noexcept {
    size_t idx = ((p >> 12) * 11400714819323198485ULL) & (PAGE_REG_SLOTS - 1);
    size_t first_tombstone = PAGE_REG_SLOTS;

    for (size_t step = 0; step < PAGE_REG_SLOTS; ++step) {
        size_t slot   = (idx + step) & (PAGE_REG_SLOTS - 1);
        uintptr_t cur = page_aligned_registry[slot].load(std::memory_order_acquire);
        if (cur == p) return true;
        if (cur == 1 && first_tombstone == PAGE_REG_SLOTS) {
            first_tombstone = slot;
        } else if (cur == 0) {
            // Confirmed p is not later in the probe chain; prefer earliest tombstone if still free
            if (first_tombstone != PAGE_REG_SLOTS) {
                uintptr_t expected_tomb = 1;
                if (page_aligned_registry[first_tombstone].compare_exchange_strong(
                        expected_tomb, p, std::memory_order_release, std::memory_order_relaxed)) {
                    return true;
                }
            }
            uintptr_t expected_zero = 0;
            if (page_aligned_registry[slot].compare_exchange_strong(
                    expected_zero, p, std::memory_order_release, std::memory_order_relaxed)) {
                return true;
            }
            // Slot raced: re-check current slot in next iteration
            --step;
        }
    }

    // Fallback pass if table has no 0 slots left (only tombstones)
    if (first_tombstone != PAGE_REG_SLOTS) {
        uintptr_t expected_tomb = 1;
        if (page_aligned_registry[first_tombstone].compare_exchange_strong(
                expected_tomb, p, std::memory_order_release, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
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
            // Race-free deletion: store tombstone (1) so concurrent readers/inserters never lose probe chains
            uintptr_t expected = p;
            page_aligned_registry[slot].compare_exchange_strong(
                expected, 1ULL, std::memory_order_release, std::memory_order_relaxed);
            return;
        }
        if (cur == 0) return;
    }
}

FLASH_ALWAYS_INLINE void track_eflash_allocation(void* ptr, size_t size) noexcept {
    uintptr_t p     = reinterpret_cast<uintptr_t>(ptr);
    size_t    span  = (size == 0) ? 16 : size;
    uintptr_t p_end = (p <= UINTPTR_MAX - span) ? (p + span) : UINTPTR_MAX;

    // Release ordering ensures cross-thread visibility on weakly ordered architectures (ARM64)
    uintptr_t cur_min = eflash_min.load(std::memory_order_relaxed);
    while (p < cur_min &&
           !eflash_min.compare_exchange_weak(cur_min, p, std::memory_order_release, std::memory_order_relaxed)) {}

    uintptr_t cur_max = eflash_max.load(std::memory_order_relaxed);
    while (p_end > cur_max &&
           !eflash_max.compare_exchange_weak(cur_max, p_end, std::memory_order_release, std::memory_order_relaxed)) {}

    if (__builtin_expect((p & (FLASH_PAGE_SIZE - 1)) == 0, 0)) {
        register_page_aligned_ptr(p);
    }
}

FLASH_ALWAYS_INLINE bool is_eflash_managed_pointer(const void* ptr) noexcept {
    if (!ptr) return false;
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);

    if (__builtin_expect((p & 15) != 0, 0)) return false;

    // Acquire ordering pairs with release CAS in track_eflash_allocation (ARM64 safe)
    if (__builtin_expect(p < eflash_min.load(std::memory_order_acquire) ||
                         p > eflash_max.load(std::memory_order_acquire), 0)) {
        return false;
    }

    // Since (p & 15) == 0 and sizeof(BlockHeader) == 16, only page-boundary pointers can cross a page when subtracting 16.
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

    // Prevent unsigned underflow when alignment is huge (e.g. 1ULL << 63 via mallocx)
    if (__builtin_expect(alignment > (std::numeric_limits<size_t>::max() >> 2), 0)) {
        errno = ENOMEM;
        return nullptr;
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
    if (__builtin_expect(!is_eflash_managed_pointer(ptr), 0)) {
        // Safely forward foreign pointers (e.g., pre-LD_PRELOAD glibc allocs) if resolver is ready
        if (in_tls_alloc == 0) {
            real_free_fn_t rf = g_real_free.load(std::memory_order_acquire);
            if (rf) {
                in_tls_alloc = 1;
                asm volatile("" ::: "memory");
                rf(ptr);
                asm volatile("" ::: "memory");
                in_tls_alloc = 0;
            }
        }
        return;
    }

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
        real_usable_size_fn_t ru = g_real_usable_size.load(std::memory_order_acquire);
        if (ru && in_tls_alloc == 0) {
            in_tls_alloc = 1;
            asm volatile("" ::: "memory");
            size_t sz = ru(const_cast<void*>(ptr));
            asm volatile("" ::: "memory");
            in_tls_alloc = 0;
            return sz;
        }
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

        if (__builtin_expect(in_tls_alloc != 0, 0)) {
            void* ptr = get_early_alloc(total_size, 16);
            if (ptr) std::memset(ptr, 0, total_size);
            return ptr;
        }

        void* ptr = safe_malloc_wrapper(total_size);
        if (__builtin_expect(!ptr, 0)) return nullptr;

        // Zero both Tier 1/2 slab allocations (<=256KB) AND Tier 3 cached large extents (<=4MB).
        // Only Tier 4 (>4MB) is guaranteed freshly mmap'd and zeroed by the OS.
        if (__builtin_expect(total_size <= FLASH_LARGE_CACHE_MAX, 1)) {
            std::memset(ptr, 0, total_size);
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
            // Safe foreign pointer realloc: delegate to RTLD_NEXT realloc to prevent truncation or over-read
            real_realloc_fn_t rr = g_real_realloc.load(std::memory_order_acquire);
            if (rr && in_tls_alloc == 0) {
                in_tls_alloc = 1;
                asm volatile("" ::: "memory");
                void* res = rr(ptr, new_size);
                asm volatile("" ::: "memory");
                in_tls_alloc = 0;
                return res;
            }
            errno = ENOMEM;
            return nullptr;
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
    FLASH_EXPORT void*  __libc_malloc(size_t size) noexcept                 { return safe_malloc_wrapper(size); }
    FLASH_EXPORT void   __libc_free(void* ptr) noexcept                     { safe_free_wrapper(ptr); }
    FLASH_EXPORT void   __libc_cfree(void* ptr) noexcept                    { safe_free_wrapper(ptr); }
    FLASH_EXPORT void*  __libc_calloc(size_t n, size_t size) noexcept       { return calloc(n, size); }
    FLASH_EXPORT void*  __libc_realloc(void* p, size_t size) noexcept       { return realloc(p, size); }
    FLASH_EXPORT void*  __libc_memalign(size_t a, size_t s) noexcept        { return memalign(a, s); }
    FLASH_EXPORT void*  __libc_valloc(size_t size) noexcept                 { return valloc(size); }
    FLASH_EXPORT void*  __libc_pvalloc(size_t size) noexcept                { return pvalloc(size); }
    FLASH_EXPORT int    __posix_memalign(void** r, size_t a, size_t s) noexcept { return posix_memalign(r, a, s); }

    // JEMALLOC EXTENDED API HOOKS
    FLASH_EXPORT void* mallocx(size_t size, int flags) noexcept {
        unsigned lg_align = static_cast<unsigned>(flags & 0x3f);
        if (lg_align >= 62) { errno = ENOMEM; return nullptr; }
        size_t align = lg_align ? (1ULL << lg_align) : 16;
        void* ptr = (align <= 16) ? safe_malloc_wrapper(size) : safe_aligned_alloc_wrapper(align, size);
        if (ptr && (flags & 0x40)) {
            std::memset(ptr, 0, size);
        }
        return ptr;
    }

    FLASH_EXPORT void* rallocx(void* ptr, size_t size, int flags) noexcept {
        unsigned lg_align = static_cast<unsigned>(flags & 0x3f);
        if (lg_align >= 62) { errno = ENOMEM; return nullptr; }
        size_t align = lg_align ? (1ULL << lg_align) : 16;

        size_t old_sz = safe_usable_size_wrapper(ptr);
        void* new_ptr = nullptr;
        if (align > 16) {
            new_ptr = safe_aligned_alloc_wrapper(align, size);
            if (!new_ptr) return nullptr;
            if (ptr && old_sz > 0) {
                std::memcpy(new_ptr, ptr, (old_sz < size) ? old_sz : size);
                safe_free_wrapper(ptr);
            }
        } else {
            new_ptr = realloc(ptr, size);
        }
        if (new_ptr && (flags & 0x40)) {
            size_t new_sz = safe_usable_size_wrapper(new_ptr);
            if (new_sz > old_sz) std::memset(static_cast<char*>(new_ptr) + old_sz, 0, new_sz - old_sz);
        }
        return new_ptr;
    }

    FLASH_EXPORT size_t xallocx(void* ptr, size_t, size_t, int) noexcept  { return safe_usable_size_wrapper(ptr); }
    FLASH_EXPORT size_t sallocx(const void* ptr, int) noexcept             { return safe_usable_size_wrapper(ptr); }
    FLASH_EXPORT void   dallocx(void* ptr, int) noexcept                   { safe_free_wrapper(ptr); }
    FLASH_EXPORT void   sdallocx(void* ptr, size_t, int) noexcept          { safe_free_wrapper(ptr); }

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
    FLASH_EXPORT void*  je_malloc(size_t size) noexcept                    { return safe_malloc_wrapper(size); }
    FLASH_EXPORT void   je_free(void* ptr) noexcept                        { safe_free_wrapper(ptr); }
    FLASH_EXPORT void*  je_calloc(size_t n, size_t s) noexcept             { return calloc(n, s); }
    FLASH_EXPORT void*  je_realloc(void* p, size_t s) noexcept             { return realloc(p, s); }
    FLASH_EXPORT void*  je_aligned_alloc(size_t a, size_t s) noexcept      { return aligned_alloc(a, s); }
    FLASH_EXPORT int    je_posix_memalign(void** r, size_t a, size_t s) noexcept { return posix_memalign(r, a, s); }
    FLASH_EXPORT void*  je_memalign(size_t a, size_t s) noexcept           { return memalign(a, s); }
    FLASH_EXPORT void*  je_valloc(size_t s) noexcept                       { return valloc(s); }
    FLASH_EXPORT size_t je_malloc_usable_size(void* ptr) noexcept          { return safe_usable_size_wrapper(ptr); }
    FLASH_EXPORT void*  je_mallocx(size_t s, int f) noexcept               { return mallocx(s, f); }
    FLASH_EXPORT void*  je_rallocx(void* p, size_t s, int f) noexcept      { return rallocx(p, s, f); }
    FLASH_EXPORT size_t je_xallocx(void* p, size_t s, size_t e, int f) noexcept { return xallocx(p, s, e, f); }
    FLASH_EXPORT size_t je_sallocx(const void* p, int f) noexcept          { return sallocx(p, f); }
    FLASH_EXPORT void   je_dallocx(void* p, int f) noexcept                { dallocx(p, f); }
    FLASH_EXPORT void   je_sdallocx(void* p, size_t s, int f) noexcept     { sdallocx(p, s, f); }
    FLASH_EXPORT size_t je_nallocx(size_t s, int f) noexcept               { return nallocx(s, f); }
    FLASH_EXPORT int    je_mallctl(const char* n, void* o, size_t* ol, void* nw, size_t nl) noexcept { return mallctl(n, o, ol, nw, nl); }
    FLASH_EXPORT int    je_mallctlnametomib(const char* n, size_t* m, size_t* ml) noexcept          { return mallctlnametomib(n, m, ml); }
    FLASH_EXPORT int    je_mallctlbymib(const size_t* m, size_t ml, void* o, size_t* ol, void* nw, size_t nl) noexcept { return mallctlbymib(m, ml, o, ol, nw, nl); }
    FLASH_EXPORT void   je_malloc_stats_print(void (*w)(void*, const char*), void* c, const char* o) noexcept { malloc_stats_print(w, c, o); }

    // Prefixed tc_* hooks
    FLASH_EXPORT void*  tc_malloc(size_t size) noexcept                    { return safe_malloc_wrapper(size); }
    FLASH_EXPORT void   tc_free(void* ptr) noexcept                        { safe_free_wrapper(ptr); }
    FLASH_EXPORT void   tc_cfree(void* ptr) noexcept                       { safe_free_wrapper(ptr); }
    FLASH_EXPORT void*  tc_calloc(size_t n, size_t s) noexcept             { return calloc(n, s); }
    FLASH_EXPORT void*  tc_realloc(void* p, size_t s) noexcept             { return realloc(p, s); }
    FLASH_EXPORT void*  tc_memalign(size_t a, size_t s) noexcept           { return memalign(a, s); }
    FLASH_EXPORT int    tc_posix_memalign(void** r, size_t a, size_t s) noexcept { return posix_memalign(r, a, s); }
    FLASH_EXPORT void*  tc_valloc(size_t s) noexcept                       { return valloc(s); }
    FLASH_EXPORT void*  tc_pvalloc(size_t s) noexcept                      { return pvalloc(s); }
    FLASH_EXPORT size_t tc_malloc_size(void* ptr) noexcept                 { return safe_usable_size_wrapper(ptr); }

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

FLASH_EXPORT void operator delete(void* ptr) noexcept                        { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr) noexcept                      { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete(void* ptr, size_t) noexcept                { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr, size_t) noexcept              { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete(void* ptr, const std::nothrow_t&) noexcept   { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr, const std::nothrow_t&) noexcept { safe_free_wrapper(ptr); }

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

FLASH_EXPORT void operator delete(void* ptr, std::align_val_t) noexcept                          { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr, std::align_val_t) noexcept                        { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete(void* ptr, size_t, std::align_val_t) noexcept                  { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr, size_t, std::align_val_t) noexcept                { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept   { safe_free_wrapper(ptr); }
FLASH_EXPORT void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { safe_free_wrapper(ptr); }

#pragma GCC diagnostic pop
