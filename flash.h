// ============================================================================
// ANANTH E-FLASH V2.0 - ULTRA-LOW-LATENCY LOCK-FREE C++ MEMORY ALLOCATOR
// Architecture:
//   Tier 1 (16B - 32KB, bins 0..11): L0 64-Bit CTZLL Bitmap Slabs + L1 Stack
//   Tier 2 (64KB - 256KB, bins 12..14): Medium 2MB Hugepage Sub-Spans
//   Tier 3 (512KB - 4MB): Lock-Free Bounded Large Extent Cache (Zero munmap)
//   Tier 4 (> 4MB): Direct OS Hugepage mmap/munmap
// Author: Mane Ram Naren (Ananth E Tech)
// Patent Pending: 202641127187
//
// LICENSE: ANANTH E-TECH FAIR-USE LICENSE
// - FREE for individuals & startups (under $2M revenue / $2M funding).
// - Commercial license required for Enterprises (HFTs, Cloud Giants, etc.).
// See LICENSE file for full terms and conditions.
// ============================================================================

#ifndef FLASH_H
#define FLASH_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <atomic>
#include <new>

#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
    #define FLASH_ARCH_64 1
#else
    #error "Ananth E-Flash V2.0 requires a 64-bit architecture."
#endif

#define FLASH_ALWAYS_INLINE __attribute__((always_inline)) inline
#define FLASH_NOINLINE      __attribute__((noinline))
#define FLASH_CACHE_LINE    64

// ==========================================
// 1. CONSTANTS & SIZE CLASSES (16B to 256KB + Cached Large up to 4MB)
// ==========================================
constexpr size_t   FLASH_PAGE_SIZE         = 4096;
constexpr size_t   FLASH_SPAN_SIZE         = 2097152; // 2MB Hugepage Spans
constexpr size_t   FLASH_MAX_FAST_ALLOC    = 262144;  // 256KB (Tier 1 + Tier 2)
constexpr uint8_t  FLASH_NUM_BINS          = 15;      // 16B .. 256KB
constexpr uint8_t  FLASH_LARGE_BIN_TAG     = 63;      // Tier 3 (Cached) & Tier 4 (Direct OS)
constexpr size_t   FLASH_LARGE_CACHE_MIN   = 524288;  // 512KB
constexpr size_t   FLASH_LARGE_CACHE_MAX   = 4194304; // 4MB
constexpr size_t   FLASH_LARGE_CACHE_BINS  = 4;       // 512KB, 1MB, 2MB, 4MB
constexpr size_t   FLASH_LARGE_CACHE_SLOTS = 8;       // Up to 8 cached extents per large bucket
constexpr size_t   FLASH_TCACHE_MAX        = 256;
constexpr size_t   FLASH_TCACHE_BATCH      = 64;
constexpr size_t   FLASH_REMOTE_BATCH_MAX  = 64;      // Bounded remote batch to avoid Producer-Consumer RSS bloat
constexpr size_t   FLASH_TRANSFER_SLOTS    = 128;
constexpr size_t   FLASH_PARTIAL_SLOTS     = 64;
constexpr size_t   FLASH_MAX_WORKERS       = 1024;

FLASH_ALWAYS_INLINE uint8_t get_size_class(size_t size) noexcept {
    if (size <= 16) return 0;
    return static_cast<uint8_t>(60 - __builtin_clzll(size - 1));
}

FLASH_ALWAYS_INLINE size_t class_to_size(uint8_t bin_idx) noexcept {
    return 1ULL << (bin_idx + 4);
}

FLASH_ALWAYS_INLINE int get_large_cache_bin(size_t bytes) noexcept {
    if (bytes == 524288ULL)  return 0;
    if (bytes == 1048576ULL) return 1;
    if (bytes == 2097152ULL) return 2;
    if (bytes == 4194304ULL) return 3;
    return -1;
}

FLASH_ALWAYS_INLINE size_t round_up_large_cache_size(size_t bytes) noexcept {
    if (bytes <= 524288ULL)  return 524288ULL;
    if (bytes <= 1048576ULL) return 1048576ULL;
    if (bytes <= 2097152ULL) return 2097152ULL;
    if (bytes <= 4194304ULL) return 4194304ULL;
    return bytes;
}

FLASH_ALWAYS_INLINE void cpu_relax() noexcept {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// ==========================================
// 2. GOD REGISTER V2.0 (64-Bit Packed Metadata)
// Exact 64-Bit Layout: 38 + 12 + 6 + 6 + 1 + 1 = 64 Bits
// ==========================================
typedef uint64_t god_reg_t;

constexpr size_t OFFSET_BITS     = 38;
constexpr size_t WORKER_BITS     = 12;
constexpr size_t NUMA_BITS       = 6;
constexpr size_t SIZE_CLASS_BITS = 6;

constexpr size_t WORKER_SHIFT     = OFFSET_BITS;
constexpr size_t NUMA_SHIFT       = WORKER_SHIFT + WORKER_BITS;
constexpr size_t SIZE_CLASS_SHIFT = NUMA_SHIFT + NUMA_BITS;
constexpr size_t ALLOCATED_SHIFT  = SIZE_CLASS_SHIFT + SIZE_CLASS_BITS;
constexpr size_t MAGIC_SHIFT      = ALLOCATED_SHIFT + 1;

constexpr god_reg_t OFFSET_MASK     = (1ULL << OFFSET_BITS) - 1;
constexpr god_reg_t WORKER_MASK     = (1ULL << WORKER_BITS) - 1;
constexpr god_reg_t NUMA_MASK       = (1ULL << NUMA_BITS) - 1;
constexpr god_reg_t SIZE_CLASS_MASK = (1ULL << SIZE_CLASS_BITS) - 1;
constexpr god_reg_t ALLOCATED_BIT   = (1ULL << ALLOCATED_SHIFT);
constexpr god_reg_t MAGIC_BIT       = (1ULL << MAGIC_SHIFT);

FLASH_ALWAYS_INLINE god_reg_t encode_god_register(
        uint64_t offset_or_pages, uint16_t worker, uint8_t numa, uint8_t size_class, bool is_allocated) noexcept {
    return (offset_or_pages & OFFSET_MASK) |
           ((static_cast<god_reg_t>(worker) & WORKER_MASK) << WORKER_SHIFT) |
           ((static_cast<god_reg_t>(numa) & NUMA_MASK) << NUMA_SHIFT) |
           ((static_cast<god_reg_t>(size_class) & SIZE_CLASS_MASK) << SIZE_CLASS_SHIFT) |
           (is_allocated ? ALLOCATED_BIT : 0ULL) | MAGIC_BIT;
}

FLASH_ALWAYS_INLINE god_reg_t set_worker_and_alloc(god_reg_t reg, uint16_t worker, bool is_allocated) noexcept {
    constexpr god_reg_t CLEAR_MASK = (WORKER_MASK << WORKER_SHIFT) | ALLOCATED_BIT;
    return (reg & ~CLEAR_MASK) | ((static_cast<god_reg_t>(worker) & WORKER_MASK) << WORKER_SHIFT) |
           (is_allocated ? ALLOCATED_BIT : 0ULL) | MAGIC_BIT;
}

FLASH_ALWAYS_INLINE uint64_t get_offset(god_reg_t reg) noexcept          { return reg & OFFSET_MASK; }
FLASH_ALWAYS_INLINE uint16_t get_worker(god_reg_t reg) noexcept          { return static_cast<uint16_t>((reg >> WORKER_SHIFT) & WORKER_MASK); }
FLASH_ALWAYS_INLINE uint8_t  get_numa_node(god_reg_t reg) noexcept       { return static_cast<uint8_t>((reg >> NUMA_SHIFT) & NUMA_MASK); }
FLASH_ALWAYS_INLINE uint8_t  get_size_class_reg(god_reg_t reg) noexcept  { return static_cast<uint8_t>((reg >> SIZE_CLASS_SHIFT) & SIZE_CLASS_MASK); }
FLASH_ALWAYS_INLINE bool     is_valid_allocated(god_reg_t reg) noexcept  { return (reg & (MAGIC_BIT | ALLOCATED_BIT)) == (MAGIC_BIT | ALLOCATED_BIT); }

// ==========================================
// 3. OS MEMORY BRIDGE
// ==========================================
FLASH_ALWAYS_INLINE void* os_page_alloc(size_t bytes) noexcept {
    if (bytes == 0 || bytes > (std::numeric_limits<size_t>::max() - FLASH_PAGE_SIZE)) return nullptr;
    size_t align_bytes = (bytes + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    void* ptr = mmap(nullptr, align_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#ifdef MADV_HUGEPAGE
    if (ptr != MAP_FAILED && align_bytes >= (2 * 1024 * 1024)) {
        madvise(ptr, align_bytes, MADV_HUGEPAGE);
    }
#endif
    return (ptr == MAP_FAILED) ? nullptr : ptr;
}

FLASH_ALWAYS_INLINE void os_page_free(void* ptr, size_t bytes) noexcept {
    if (ptr && bytes > 0) {
        size_t align_bytes = (bytes + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
        munmap(ptr, align_bytes);
    }
}

// ==========================================
// 4. BLOCK HEADER & LOCK-FREE CENTRAL SPAN
// 21-Bit Span Offset = Supports Full 2MB (2,097,152 B) Spans + 42-Bit ABA Tag
// ==========================================
struct CentralSpan;
struct alignas(16) BlockHeader {
    std::atomic<god_reg_t> god_tag;
    CentralSpan*           parent_span;
};
static_assert(sizeof(BlockHeader) == 16, "BlockHeader must remain 16 bytes for MMU page-boundary safety.");

constexpr uint64_t SPAN_OFFSET_BITS = 21;
constexpr uint64_t SPAN_OFFSET_MASK = (1ULL << SPAN_OFFSET_BITS) - 1;
constexpr uint64_t SPAN_VALID_BIT   = (1ULL << SPAN_OFFSET_BITS);
constexpr uint64_t SPAN_TAG_STEP    = (1ULL << (SPAN_OFFSET_BITS + 1));

struct alignas(FLASH_CACHE_LINE) CentralSpan {
    void*                 page_start;
    std::atomic<uint64_t> free_list_tagged;
    std::atomic<uint32_t> allocated_count;
    std::atomic<uint32_t> reclaimed_flag;
    uint32_t              total_blocks;
    uint32_t              bump_index;
    uint16_t              owner_worker;
    uint8_t               numa_node;
    uint8_t               size_class;
    std::atomic<uint32_t> partial_state; // 0 = Detached, 1 = In partial_spans, 2 = In active_span
    uint8_t               padding[24];
};

// ==========================================
// 5. THE BEAST: L0 BITMAP, L1 ARRAY & TIER 3 LARGE CACHE ENGINE
// ==========================================
struct alignas(FLASH_CACHE_LINE) LockFreeTransferCache {
    std::atomic<uintptr_t> slots[FLASH_TRANSFER_SLOTS];
};

struct alignas(FLASH_CACHE_LINE) LockFreePartialCache {
    std::atomic<CentralSpan*> slots[FLASH_PARTIAL_SLOTS];
};

struct alignas(FLASH_CACHE_LINE) LockFreeLargeCache {
    std::atomic<void*> slots[FLASH_LARGE_CACHE_SLOTS];
};

struct alignas(FLASH_CACHE_LINE) WorkerEngine {
    uint16_t                  worker_id;
    uint8_t                   numa_node;
    uint8_t                   pad0;
    std::atomic<uint32_t>     active_users; // Ref-counted for safe >1024 threads sharing
    std::atomic<CentralSpan*> cached_empty_span[FLASH_NUM_BINS];
    LockFreePartialCache      partial_spans[FLASH_NUM_BINS];
    LockFreeTransferCache     transfer_cache[FLASH_NUM_BINS];
    alignas(FLASH_CACHE_LINE) std::atomic<uintptr_t> remote_free_head;
};

struct alignas(FLASH_CACHE_LINE) ZoneMaster {
    uint16_t                  total_workers;
    uint16_t                  hardware_cores;
    god_reg_t                 canary_seed;
    pthread_key_t             tls_cleanup_key;
    alignas(FLASH_CACHE_LINE) std::atomic<uint16_t> worker_ticket;
    alignas(FLASH_CACHE_LINE) LockFreeLargeCache    large_cache[FLASH_LARGE_CACHE_BINS];
    alignas(FLASH_CACHE_LINE) WorkerEngine          workers[FLASH_MAX_WORKERS];
};

inline std::atomic<uint8_t> EFlash_State{0};
inline ZoneMaster*          G_Zone = nullptr;

// THE HYBRID THREAD CACHE (L0 BITMAP + L1 ARRAY STACK)
struct alignas(FLASH_CACHE_LINE) FastCache {
    WorkerEngine* worker;
    uint16_t      worker_id;
    bool          initialized;
    bool          torn_down;

    // L0 Cache: 64-Bit CTZLL Bitmap Slabs (ZERO Pointer Chasing)
    uint64_t      slab_bitmap[FLASH_NUM_BINS];
    uintptr_t     slab_base[FLASH_NUM_BINS];
    uint8_t       slab_count[FLASH_NUM_BINS]; // Exact carved count (<= 64) to prevent cross-span L0 aliasing

    // L1 Cache: Fast Array Stack (ZERO Payload Touching)
    uint16_t      count[FLASH_NUM_BINS];
    uintptr_t     free_stack[FLASH_NUM_BINS][FLASH_TCACHE_MAX];

    CentralSpan*  active_span[FLASH_NUM_BINS];

    uintptr_t     batch_head;
    uintptr_t     batch_tail;
    uint16_t      batch_owner;
    uint16_t      batch_count;
};

inline __thread FastCache t_cache __attribute__((tls_model("initial-exec")));

FLASH_ALWAYS_INLINE void release_span_reference_lockfree(CentralSpan* span) noexcept;
FLASH_ALWAYS_INLINE void try_queue_partial_span_lockfree(CentralSpan* span) noexcept;
FLASH_ALWAYS_INLINE void return_block_to_span_lockfree(uintptr_t user_addr, BlockHeader* hdr) noexcept;
FLASH_ALWAYS_INLINE void drain_tcache_bin(uint8_t bin, uint16_t count_to_drain) noexcept;
FLASH_ALWAYS_INLINE void drain_l0_slab(uint8_t bin) noexcept;
FLASH_ALWAYS_INLINE void flush_remote_batch() noexcept;

inline void eflash_pthread_Exit_Callback(void*) noexcept {
    t_cache.torn_down = true;
    if (EFlash_State.load(std::memory_order_acquire) != 2 || !G_Zone || !t_cache.worker) return;

    if (t_cache.batch_count > 0) flush_remote_batch();

    WorkerEngine* w = t_cache.worker;

    for (uint8_t b = 0; b < FLASH_NUM_BINS; ++b) {
        // 1. Drain L1 Array Stack
        if (t_cache.count[b] > 0) drain_tcache_bin(b, t_cache.count[b]);

        // 2. Drain L0 Bitmap Slab
        drain_l0_slab(b);

        // 3. Release Active Span Reference (queue into partial_spans if it has unharvested free blocks)
        CentralSpan* span = t_cache.active_span[b];
        if (span != nullptr) {
            span->partial_state.store(0U, std::memory_order_seq_cst);
            if ((span->free_list_tagged.load(std::memory_order_seq_cst) & SPAN_VALID_BIT) != 0ULL) {
                try_queue_partial_span_lockfree(span);
            }
            release_span_reference_lockfree(span);
            t_cache.active_span[b] = nullptr;
        }
    }

    // Decrement active_users with seq_cst barrier against flush_remote_batch
    if (w->active_users.fetch_sub(1, std::memory_order_seq_cst) == 1) {
        // Last thread on this worker: drain transfer_cache batches so no spans stay stranded
        for (uint8_t b = 0; b < FLASH_NUM_BINS; ++b) {
            LockFreeTransferCache& tc = w->transfer_cache[b];
            for (size_t s = 0; s < FLASH_TRANSFER_SLOTS; ++s) {
                uintptr_t batch_head = tc.slots[s].exchange(0, std::memory_order_acquire);
                while (batch_head != 0) {
                    uint64_t* payload = reinterpret_cast<uint64_t*>(batch_head);
                    uintptr_t next = (*payload) ^ G_Zone->canary_seed;
                    BlockHeader* hdr = reinterpret_cast<BlockHeader*>(batch_head) - 1;
                    return_block_to_span_lockfree(batch_head, hdr);
                    batch_head = next;
                }
            }
        }

        // Drain remote mailbox
        uintptr_t rem = w->remote_free_head.exchange(0, std::memory_order_seq_cst);
        while (rem != 0) {
            uint64_t* p = reinterpret_cast<uint64_t*>(rem);
            uintptr_t next = (*p) ^ G_Zone->canary_seed;
            BlockHeader* hdr = reinterpret_cast<BlockHeader*>(rem) - 1;
            return_block_to_span_lockfree(rem, hdr);
            rem = next;
        }

        // Release any idle partial span slot references if completely unused
        for (uint8_t b = 0; b < FLASH_NUM_BINS; ++b) {
            LockFreePartialCache& pc = w->partial_spans[b];
            for (size_t s = 0; s < FLASH_PARTIAL_SLOTS; ++s) {
                CentralSpan* pspan = pc.slots[s].exchange(nullptr, std::memory_order_acquire);
                if (pspan != nullptr) {
                    pspan->partial_state.store(0U, std::memory_order_release);
                    release_span_reference_lockfree(pspan);
                }
            }
        }
    }

    t_cache.worker = nullptr;
}

// ==========================================
// 6. BOOT SEQUENCE
// ==========================================
inline void init_eflash_boot_sequence() noexcept {
    uint8_t expected = 0;
    if (EFlash_State.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        G_Zone = static_cast<ZoneMaster*>(os_page_alloc(sizeof(ZoneMaster)));
        if (!G_Zone) {
            EFlash_State.store(0, std::memory_order_release);
            return;
        }
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        G_Zone->hardware_cores = (cpus > 0) ? static_cast<uint16_t>(cpus) : 4;
        G_Zone->total_workers  = static_cast<uint16_t>(FLASH_MAX_WORKERS);
        G_Zone->worker_ticket.store(0, std::memory_order_relaxed);

        uintptr_t entropy = reinterpret_cast<uintptr_t>(G_Zone) ^ static_cast<uintptr_t>(getpid());
        G_Zone->canary_seed = static_cast<god_reg_t>((entropy << 13) ^ (entropy >> 7) ^ 0xA5A5F0F09696ULL) | 1ULL;

        for (size_t lb = 0; lb < FLASH_LARGE_CACHE_BINS; ++lb) {
            for (size_t s = 0; s < FLASH_LARGE_CACHE_SLOTS; ++s) {
                G_Zone->large_cache[lb].slots[s].store(nullptr, std::memory_order_relaxed);
            }
        }

        pthread_key_create(&G_Zone->tls_cleanup_key, eflash_pthread_Exit_Callback);
        for (size_t i = 0; i < FLASH_MAX_WORKERS; ++i) {
            WorkerEngine* w = &G_Zone->workers[i];
            w->worker_id = static_cast<uint16_t>(i);
            w->numa_node = 0;
            w->pad0      = 0;
            w->active_users.store(0, std::memory_order_relaxed);
            w->remote_free_head.store(0, std::memory_order_relaxed);
            for (uint8_t b = 0; b < FLASH_NUM_BINS; ++b) {
                w->cached_empty_span[b].store(nullptr, std::memory_order_relaxed);
                for (size_t s = 0; s < FLASH_PARTIAL_SLOTS; ++s) {
                    w->partial_spans[b].slots[s].store(nullptr, std::memory_order_relaxed);
                }
                for (size_t s = 0; s < FLASH_TRANSFER_SLOTS; ++s) {
                    w->transfer_cache[b].slots[s].store(0, std::memory_order_relaxed);
                }
            }
        }
        EFlash_State.store(2, std::memory_order_release);
    } else {
        while (EFlash_State.load(std::memory_order_acquire) == 1) cpu_relax();
    }
}

// ==========================================
// 7. THREAD INITIALIZATION
// ==========================================
FLASH_ALWAYS_INLINE void init_thread_worker() noexcept {
    if (__builtin_expect(EFlash_State.load(std::memory_order_acquire) != 2, 0)) init_eflash_boot_sequence();
    if (__builtin_expect(!G_Zone, 0)) return;

    int cpu = sched_getcpu();
    uint16_t start_slot = (cpu >= 0)
        ? static_cast<uint16_t>(cpu % FLASH_MAX_WORKERS)
        : static_cast<uint16_t>(G_Zone->worker_ticket.fetch_add(1, std::memory_order_relaxed) % FLASH_MAX_WORKERS);

    uint16_t chosen_slot = start_slot;
    bool claimed_exclusive = false;
    for (size_t step = 0; step < FLASH_MAX_WORKERS; ++step) {
        uint16_t cand = static_cast<uint16_t>((start_slot + step) % FLASH_MAX_WORKERS);
        uint32_t expected = 0;
        if (G_Zone->workers[cand].active_users.compare_exchange_strong(
                expected, 1U, std::memory_order_acquire, std::memory_order_relaxed)) {
            chosen_slot = cand;
            claimed_exclusive = true;
            break;
        }
    }

    if (__builtin_expect(!claimed_exclusive, 0)) {
        G_Zone->workers[chosen_slot].active_users.fetch_add(1U, std::memory_order_acq_rel);
    }

    t_cache.worker_id = chosen_slot;
    t_cache.worker    = &G_Zone->workers[chosen_slot];

    for (int i = 0; i < FLASH_NUM_BINS; ++i) {
        t_cache.slab_bitmap[i] = ~0ULL;
        t_cache.slab_base[i]   = 0;
        t_cache.slab_count[i]  = 0;
        t_cache.count[i]       = 0;
        t_cache.active_span[i] = nullptr;
    }
    asm volatile("" ::: "memory");
    t_cache.initialized = true;
    pthread_setspecific(G_Zone->tls_cleanup_key, reinterpret_cast<void*>(1));
}

// ==========================================
// 8. LOCK-FREE RECLAMATION, PARTIAL SPANS, LARGE CACHE & L1 MANAGEMENT
// ==========================================
FLASH_ALWAYS_INLINE void* pop_large_extent_cache(size_t bucket_bytes) noexcept {
    if (__builtin_expect(!G_Zone, 0)) return nullptr;
    int lb = get_large_cache_bin(bucket_bytes);
    if (lb < 0) return nullptr;

    LockFreeLargeCache& lc = G_Zone->large_cache[lb];
    for (size_t s = 0; s < FLASH_LARGE_CACHE_SLOTS; ++s) {
        void* p = lc.slots[s].load(std::memory_order_relaxed);
        if (p != nullptr && lc.slots[s].compare_exchange_strong(
                p, nullptr, std::memory_order_acquire, std::memory_order_relaxed)) {
            return p;
        }
    }
    return nullptr;
}

FLASH_ALWAYS_INLINE bool push_large_extent_cache(void* raw_mem, size_t bucket_bytes) noexcept {
    if (__builtin_expect(!G_Zone || !raw_mem, 0)) return false;
    int lb = get_large_cache_bin(bucket_bytes);
    if (lb < 0) return false;

    LockFreeLargeCache& lc = G_Zone->large_cache[lb];
    for (size_t s = 0; s < FLASH_LARGE_CACHE_SLOTS; ++s) {
        void* expected = nullptr;
        if (lc.slots[s].load(std::memory_order_relaxed) == nullptr &&
            lc.slots[s].compare_exchange_strong(
                expected, raw_mem, std::memory_order_release, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

FLASH_ALWAYS_INLINE void release_span_reference_lockfree(CentralSpan* span) noexcept {
    uint32_t prev = span->allocated_count.fetch_sub(1U, std::memory_order_acq_rel);

    if (__builtin_expect(prev == 2U && span->partial_state.load(std::memory_order_acquire) == 1U, 0)) {
        WorkerEngine* owner_w = &G_Zone->workers[span->owner_worker % FLASH_MAX_WORKERS];
        LockFreePartialCache& pc = owner_w->partial_spans[span->size_class];
        for (size_t s = 0; s < FLASH_PARTIAL_SLOTS; ++s) {
            CentralSpan* expected_span = span;
            if (pc.slots[s].load(std::memory_order_relaxed) == span &&
                pc.slots[s].compare_exchange_strong(
                    expected_span, nullptr, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                span->partial_state.store(0U, std::memory_order_release);
                if (span->allocated_count.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
                    prev = 1U;
                }
                break;
            }
        }
    }

    if (prev == 1U) {
        uint32_t expected = 0U;
        if (span->reclaimed_flag.compare_exchange_strong(expected, 1U, std::memory_order_acq_rel)) {
            WorkerEngine* owner_w = &G_Zone->workers[span->owner_worker % FLASH_MAX_WORKERS];
            CentralSpan* prev_cached = owner_w->cached_empty_span[span->size_class].exchange(span, std::memory_order_acq_rel);
            if (prev_cached != nullptr) os_page_free(prev_cached->page_start, FLASH_SPAN_SIZE);
        }
    }
}

FLASH_ALWAYS_INLINE void try_queue_partial_span_lockfree(CentralSpan* span) noexcept {
    if (span->partial_state.load(std::memory_order_relaxed) != 0U) return;
    if (span->allocated_count.load(std::memory_order_relaxed) <= 1U) return;

    uint32_t expected_state = 0U;
    if (!span->partial_state.compare_exchange_strong(
            expected_state, 1U, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return;
    }

    span->allocated_count.fetch_add(1U, std::memory_order_acq_rel);

    WorkerEngine* owner_w = &G_Zone->workers[span->owner_worker % FLASH_MAX_WORKERS];
    LockFreePartialCache& pc = owner_w->partial_spans[span->size_class];

    for (size_t s = 0; s < FLASH_PARTIAL_SLOTS; ++s) {
        CentralSpan* expected_slot = nullptr;
        if (pc.slots[s].load(std::memory_order_relaxed) == nullptr &&
            pc.slots[s].compare_exchange_strong(
                expected_slot, span, std::memory_order_release, std::memory_order_relaxed)) {
            return;
        }
    }

    span->partial_state.store(0U, std::memory_order_release);
    release_span_reference_lockfree(span);
}

FLASH_ALWAYS_INLINE void repush_unharvested_block_lockfree(CentralSpan* span, uintptr_t user_addr) noexcept {
    uintptr_t span_base = reinterpret_cast<uintptr_t>(span->page_start);
    uint64_t block_offset = static_cast<uint64_t>(user_addr - span_base);

    uint64_t* payload = reinterpret_cast<uint64_t*>(user_addr);
    uint64_t old_head = span->free_list_tagged.load(std::memory_order_relaxed);
    uint64_t new_head = 0;

    do {
        uintptr_t next_ptr = (old_head & SPAN_VALID_BIT) ? (span_base + (old_head & SPAN_OFFSET_MASK)) : 0ULL;
        *payload = next_ptr ^ G_Zone->canary_seed;
        uint64_t next_tag = (old_head & ~(SPAN_OFFSET_MASK | SPAN_VALID_BIT)) + SPAN_TAG_STEP;
        new_head = next_tag | SPAN_VALID_BIT | (block_offset & SPAN_OFFSET_MASK);
    } while (!span->free_list_tagged.compare_exchange_weak(
        old_head, new_head, std::memory_order_release, std::memory_order_relaxed));
}

FLASH_ALWAYS_INLINE void return_block_to_span_lockfree(uintptr_t user_addr, BlockHeader* hdr) noexcept {
    CentralSpan* span = hdr->parent_span;
    repush_unharvested_block_lockfree(span, user_addr);
    if (span->partial_state.load(std::memory_order_relaxed) == 0U &&
        span->allocated_count.load(std::memory_order_relaxed) > 1U) {
        try_queue_partial_span_lockfree(span);
    }
    release_span_reference_lockfree(span);
}

FLASH_ALWAYS_INLINE uint16_t harvest_span_freelist_lockfree(CentralSpan* span, uint8_t bin, uint16_t local_worker_id) noexcept {
    uint64_t old_head = span->free_list_tagged.load(std::memory_order_acquire);
    while (old_head & SPAN_VALID_BIT) {
        uint64_t next_tag = (old_head & ~(SPAN_OFFSET_MASK | SPAN_VALID_BIT)) + SPAN_TAG_STEP;
        if (span->free_list_tagged.compare_exchange_weak(old_head, next_tag, std::memory_order_acquire, std::memory_order_relaxed)) {
            uintptr_t span_base = reinterpret_cast<uintptr_t>(span->page_start);
            uintptr_t curr = span_base + (old_head & SPAN_OFFSET_MASK);
            uint16_t harvested = 0;

            while (curr != 0 && t_cache.count[bin] < FLASH_TCACHE_MAX) {
                uint64_t* payload = reinterpret_cast<uint64_t*>(curr);
                uintptr_t next = (*payload) ^ G_Zone->canary_seed;
                BlockHeader* hdr = reinterpret_cast<BlockHeader*>(curr) - 1;

                god_reg_t old_tag = hdr->god_tag.load(std::memory_order_relaxed);
                hdr->god_tag.store(set_worker_and_alloc(old_tag, local_worker_id, false), std::memory_order_relaxed);
                span->allocated_count.fetch_add(1, std::memory_order_relaxed);

                t_cache.free_stack[bin][t_cache.count[bin]++] = curr;
                harvested++;
                curr = next;
            }

            while (curr != 0) {
                uint64_t* payload = reinterpret_cast<uint64_t*>(curr);
                uintptr_t next = (*payload) ^ G_Zone->canary_seed;
                repush_unharvested_block_lockfree(span, curr);
                curr = next;
            }
            return harvested;
        }
    }
    return 0;
}

FLASH_ALWAYS_INLINE bool pop_partial_span_batch(WorkerEngine* w, uint8_t bin) noexcept {
    LockFreePartialCache& pc = w->partial_spans[bin];
    for (size_t s = 0; s < FLASH_PARTIAL_SLOTS; ++s) {
        CentralSpan* span = pc.slots[s].load(std::memory_order_relaxed);
        if (span != nullptr &&
            pc.slots[s].compare_exchange_strong(
                span, nullptr, std::memory_order_acquire, std::memory_order_relaxed)) {
            uint16_t got = harvest_span_freelist_lockfree(span, bin, t_cache.worker_id);
            span->partial_state.store(0U, std::memory_order_seq_cst);
            if ((span->free_list_tagged.load(std::memory_order_seq_cst) & SPAN_VALID_BIT) != 0ULL) {
                try_queue_partial_span_lockfree(span);
            }
            release_span_reference_lockfree(span);
            if (got > 0) return true;
        }
    }
    return false;
}

FLASH_ALWAYS_INLINE void drain_l0_slab(uint8_t b) noexcept {
    uint64_t bmp = t_cache.slab_bitmap[b];
    if (bmp != ~0ULL && t_cache.slab_base[b] != 0) {
        uintptr_t slab_base = t_cache.slab_base[b];
        uint32_t limit = t_cache.slab_count[b];
        size_t stride = sizeof(BlockHeader) + class_to_size(b);
        for (uint32_t i = 0; i < limit; ++i) {
            if ((bmp & (1ULL << i)) == 0) {
                uintptr_t user_addr = slab_base + (i * stride);
                BlockHeader* hdr = reinterpret_cast<BlockHeader*>(user_addr) - 1;
                return_block_to_span_lockfree(user_addr, hdr);
            }
        }
        t_cache.slab_bitmap[b] = ~0ULL;
        t_cache.slab_base[b]   = 0;
        t_cache.slab_count[b]  = 0;
    }
}

FLASH_ALWAYS_INLINE void drain_tcache_bin(uint8_t bin, uint16_t count_to_drain) noexcept {
    if (count_to_drain == 0 || t_cache.count[bin] < count_to_drain) return;

    uintptr_t batch_head = 0;
    for (uint16_t i = 0; i < count_to_drain; ++i) {
        uintptr_t curr = t_cache.free_stack[bin][t_cache.count[bin] - 1 - i];
        uint64_t* payload = reinterpret_cast<uint64_t*>(curr);
        *payload = (i == 0) ? (0ULL ^ G_Zone->canary_seed) : (batch_head ^ G_Zone->canary_seed);
        batch_head = curr;
    }
    t_cache.count[bin] -= count_to_drain;

    WorkerEngine* w = t_cache.worker;
    if (w && !t_cache.torn_down && count_to_drain == FLASH_TCACHE_BATCH) {
        LockFreeTransferCache& tc = w->transfer_cache[bin];
        for (size_t s = 0; s < FLASH_TRANSFER_SLOTS; ++s) {
            uintptr_t expected = 0;
            if (tc.slots[s].load(std::memory_order_relaxed) == 0 &&
                tc.slots[s].compare_exchange_strong(expected, batch_head, std::memory_order_release, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    uintptr_t node = batch_head;
    while (node != 0) {
        uint64_t* payload = reinterpret_cast<uint64_t*>(node);
        uintptr_t next = (*payload) ^ G_Zone->canary_seed;
        BlockHeader* hdr = reinterpret_cast<BlockHeader*>(node) - 1;
        return_block_to_span_lockfree(node, hdr);
        node = next;
    }
}

FLASH_ALWAYS_INLINE bool pop_transfer_cache_batch(WorkerEngine* w, uint8_t bin) noexcept {
    LockFreeTransferCache& tc = w->transfer_cache[bin];
    for (size_t s = 0; s < FLASH_TRANSFER_SLOTS; ++s) {
        uintptr_t batch_head = tc.slots[s].load(std::memory_order_relaxed);
        if (batch_head != 0 && tc.slots[s].compare_exchange_strong(batch_head, 0, std::memory_order_acquire, std::memory_order_relaxed)) {
            uintptr_t curr = batch_head;
            while (curr != 0 && t_cache.count[bin] < FLASH_TCACHE_MAX) {
                uint64_t* payload = reinterpret_cast<uint64_t*>(curr);
                uintptr_t next = (*payload) ^ G_Zone->canary_seed;
                BlockHeader* hdr = reinterpret_cast<BlockHeader*>(curr) - 1;
                god_reg_t old_tag = hdr->god_tag.load(std::memory_order_relaxed);
                hdr->god_tag.store(set_worker_and_alloc(old_tag, t_cache.worker_id, false), std::memory_order_relaxed);

                t_cache.free_stack[bin][t_cache.count[bin]++] = curr;
                curr = next;
            }
            while (curr != 0) {
                uint64_t* payload = reinterpret_cast<uint64_t*>(curr);
                uintptr_t next = (*payload) ^ G_Zone->canary_seed;
                BlockHeader* hdr = reinterpret_cast<BlockHeader*>(curr) - 1;
                return_block_to_span_lockfree(curr, hdr);
                curr = next;
            }
            return true;
        }
    }
    return false;
}

FLASH_ALWAYS_INLINE void flush_remote_batch() noexcept {
    if (__builtin_expect(t_cache.batch_count == 0, 1)) return;
    WorkerEngine* target = &G_Zone->workers[t_cache.batch_owner];
    uint64_t* tail_payload = reinterpret_cast<uint64_t*>(t_cache.batch_tail);
    uintptr_t old_head = target->remote_free_head.load(std::memory_order_relaxed);

    do { *tail_payload = old_head ^ G_Zone->canary_seed; }
    while (!target->remote_free_head.compare_exchange_weak(
        old_head, t_cache.batch_head, std::memory_order_seq_cst, std::memory_order_relaxed));

    t_cache.batch_head = 0;
    t_cache.batch_tail = 0;
    t_cache.batch_count = 0;

    if (__builtin_expect(target->active_users.load(std::memory_order_seq_cst) == 0, 0)) {
        uintptr_t orphan = target->remote_free_head.exchange(0, std::memory_order_seq_cst);
        while (orphan != 0) {
            uint64_t* p = reinterpret_cast<uint64_t*>(orphan);
            uintptr_t next = (*p) ^ G_Zone->canary_seed;
            BlockHeader* hdr = reinterpret_cast<BlockHeader*>(orphan) - 1;
            return_block_to_span_lockfree(orphan, hdr);
            orphan = next;
        }
    }
}

FLASH_ALWAYS_INLINE void drain_remote_mailbox(WorkerEngine* w) noexcept {
    uintptr_t curr = w->remote_free_head.exchange(0, std::memory_order_acquire);
    while (curr != 0) {
        uint64_t* payload = reinterpret_cast<uint64_t*>(curr);
        uintptr_t next = (*payload) ^ G_Zone->canary_seed;
        BlockHeader* hdr = reinterpret_cast<BlockHeader*>(curr) - 1;
        god_reg_t old_tag = hdr->god_tag.load(std::memory_order_relaxed);
        uint8_t b = get_size_class_reg(old_tag);

        if (b < FLASH_NUM_BINS && t_cache.count[b] < FLASH_TCACHE_MAX) {
            hdr->god_tag.store(set_worker_and_alloc(old_tag, t_cache.worker_id, false), std::memory_order_relaxed);
            t_cache.free_stack[b][t_cache.count[b]++] = curr;
        } else {
            return_block_to_span_lockfree(curr, hdr);
        }
        curr = next;
    }
}

// L0 BITMAP SLAB CARVER (Page-Boundary Skip + Partial Span Harvest)
FLASH_ALWAYS_INLINE void refill_tcache_from_spans(uint8_t bin) noexcept {
    WorkerEngine* w = t_cache.worker;
    if (pop_transfer_cache_batch(w, bin)) return;

    size_t payload_size = class_to_size(bin);
    size_t stride       = sizeof(BlockHeader) + payload_size;

    while (t_cache.slab_bitmap[bin] == ~0ULL && t_cache.count[bin] == 0) {
        CentralSpan* span = t_cache.active_span[bin];

        if (span != nullptr) {
            if (harvest_span_freelist_lockfree(span, bin, t_cache.worker_id) > 0) return;
            if (span->bump_index >= span->total_blocks) {
                span->partial_state.store(0U, std::memory_order_seq_cst);
                if ((span->free_list_tagged.load(std::memory_order_seq_cst) & SPAN_VALID_BIT) != 0ULL) {
                    try_queue_partial_span_lockfree(span);
                }
                release_span_reference_lockfree(span);
                t_cache.active_span[bin] = nullptr;
                span = nullptr;
            }
        }

        if (span == nullptr) {
            if (pop_partial_span_batch(w, bin)) return;

            span = w->cached_empty_span[bin].exchange(nullptr, std::memory_order_acquire);
            if (!span) {
                void* raw_page = os_page_alloc(FLASH_SPAN_SIZE);
                if (__builtin_expect(!raw_page, 0)) return;
                span = static_cast<CentralSpan*>(raw_page);
                span->page_start = raw_page;
            }
            span->free_list_tagged.store(0ULL, std::memory_order_relaxed);
            span->allocated_count.store(1U, std::memory_order_relaxed);
            span->reclaimed_flag.store(0U, std::memory_order_relaxed);
            span->total_blocks = static_cast<uint32_t>((FLASH_SPAN_SIZE - sizeof(CentralSpan)) / stride);
            span->bump_index   = 0;
            span->owner_worker = t_cache.worker_id;
            span->numa_node    = w->numa_node;
            span->size_class   = bin;
            span->partial_state.store(2U, std::memory_order_relaxed);
            t_cache.active_span[bin] = span;
        }

        uint32_t remaining_blocks = span->total_blocks - span->bump_index;
        uint32_t to_carve         = (remaining_blocks < 64) ? remaining_blocks : 64;

        char* base_cursor = reinterpret_cast<char*>(span->page_start) + sizeof(CentralSpan);
        char* first_block = base_cursor + (static_cast<size_t>(span->bump_index) * stride);
        span->bump_index += to_carve;

        uint64_t invalid_mask = 0ULL;
        uint32_t valid_carved = 0U;

        for (uint32_t i = 0; i < to_carve; ++i) {
            char* block_addr    = first_block + (i * stride);
            uintptr_t user_addr = reinterpret_cast<uintptr_t>(block_addr) + sizeof(BlockHeader);
            BlockHeader* hdr    = reinterpret_cast<BlockHeader*>(block_addr);

            if (__builtin_expect((user_addr & (FLASH_PAGE_SIZE - 1)) == 0, 0)) {
                invalid_mask |= (1ULL << i);
                hdr->god_tag.store(0ULL, std::memory_order_relaxed);
                hdr->parent_span = span;
                continue;
            }

            uint64_t offset = static_cast<uint64_t>(block_addr - reinterpret_cast<char*>(span->page_start));
            hdr->god_tag.store(encode_god_register(offset, t_cache.worker_id, w->numa_node, bin, false), std::memory_order_relaxed);
            hdr->parent_span = span;
            valid_carved++;
        }

        if (valid_carved > 0U) {
            span->allocated_count.fetch_add(valid_carved, std::memory_order_relaxed);
            t_cache.slab_base[bin]   = reinterpret_cast<uintptr_t>(first_block) + sizeof(BlockHeader);
            t_cache.slab_count[bin]  = static_cast<uint8_t>(to_carve);
            uint64_t valid_bits_mask = (to_carve == 64) ? ~0ULL : ((1ULL << to_carve) - 1ULL);
            t_cache.slab_bitmap[bin] = (~valid_bits_mask) | invalid_mask;
        }

        if (span->bump_index >= span->total_blocks) {
            span->partial_state.store(0U, std::memory_order_seq_cst);
            if ((span->free_list_tagged.load(std::memory_order_seq_cst) & SPAN_VALID_BIT) != 0ULL) {
                try_queue_partial_span_lockfree(span);
            }
            release_span_reference_lockfree(span);
            t_cache.active_span[bin] = nullptr;
        }
    }
}

// ==========================================
// 9. CORE ALLOCATION & FREE APIS (TIER 1-4 HYBRID ENGINE)
// ==========================================
FLASH_ALWAYS_INLINE void* eflash_aligned_alloc_core(size_t alignment, size_t size) noexcept {
    if (alignment < 16) alignment = 16;
    if ((alignment & (alignment - 1)) != 0) {
        if (alignment > (std::numeric_limits<size_t>::max() >> 1)) return nullptr;
        alignment = 1ULL << (64 - __builtin_clzll(alignment - 1));
    }
    if (alignment > (std::numeric_limits<size_t>::max() >> 2)) return nullptr;
    if (__builtin_expect(EFlash_State.load(std::memory_order_acquire) != 2, 0)) init_eflash_boot_sequence();

    size_t actual_size = (size == 0) ? 16 : size;
    if (actual_size > std::numeric_limits<size_t>::max() - alignment - sizeof(BlockHeader) - FLASH_PAGE_SIZE) {
        return nullptr;
    }

    size_t total_request = actual_size + alignment + sizeof(BlockHeader);
    size_t aligned_bytes = (total_request + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);

    void* raw_mem = nullptr;
    // Tier 3: Bounded Large Extent Cache for 512KB .. 4MB (zero mmap/munmap churn)
    if (alignment <= FLASH_PAGE_SIZE && aligned_bytes <= FLASH_LARGE_CACHE_MAX) {
        size_t bucket_bytes = round_up_large_cache_size(aligned_bytes);
        if (bucket_bytes >= FLASH_LARGE_CACHE_MIN && bucket_bytes <= FLASH_LARGE_CACHE_MAX) {
            aligned_bytes = bucket_bytes;
            raw_mem = pop_large_extent_cache(bucket_bytes);
        }
    }

    if (!raw_mem) {
        raw_mem = os_page_alloc(aligned_bytes);
        if (__builtin_expect(!raw_mem, 0)) return nullptr;
    }

    uintptr_t raw_base  = reinterpret_cast<uintptr_t>(raw_mem);
    uintptr_t user_addr = (raw_base + sizeof(BlockHeader) + (alignment - 1)) & ~(alignment - 1);

    BlockHeader* hdr     = reinterpret_cast<BlockHeader*>(user_addr) - 1;
    uint64_t total_pages = aligned_bytes >> 12;
    hdr->parent_span     = reinterpret_cast<CentralSpan*>(raw_mem);
    hdr->god_tag.store(encode_god_register(total_pages, 0, 0, FLASH_LARGE_BIN_TAG, true), std::memory_order_release);

    return reinterpret_cast<void*>(user_addr);
}

FLASH_ALWAYS_INLINE void* eflash_malloc_core(size_t size) noexcept {
    size_t safe_size = (__builtin_expect(size == 0, 0)) ? 16 : size;
    if (__builtin_expect(safe_size > FLASH_MAX_FAST_ALLOC, 0)) return eflash_aligned_alloc_core(16, safe_size);
    if (__builtin_expect(!t_cache.initialized, 0)) init_thread_worker();
    if (__builtin_expect(!t_cache.worker, 0)) return nullptr;

    uint8_t bin = get_size_class(safe_size);

    // 1. O(1) L0 CTZLL BITMAP CACHE (Zero Cache Misses)
    uint64_t bmp = t_cache.slab_bitmap[bin];
    if (__builtin_expect(bmp != ~0ULL, 1)) {
        uint32_t idx = __builtin_ctzll(~bmp);
        t_cache.slab_bitmap[bin] = bmp | (1ULL << idx);

        size_t stride = sizeof(BlockHeader) + class_to_size(bin);
        uintptr_t user_addr = t_cache.slab_base[bin] + (idx * stride);
        BlockHeader* hdr = reinterpret_cast<BlockHeader*>(user_addr) - 1;

        god_reg_t old_tag = hdr->god_tag.load(std::memory_order_relaxed);
        hdr->god_tag.store(set_worker_and_alloc(old_tag, t_cache.worker_id, true), std::memory_order_release);
        return reinterpret_cast<void*>(user_addr);
    }

    // 2. L1 ARRAY STACK (Zero Pointer Chasing)
    if (__builtin_expect(t_cache.count[bin] > 0, 1)) {
        t_cache.count[bin]--;
        uintptr_t target = t_cache.free_stack[bin][t_cache.count[bin]];
        BlockHeader* hdr = reinterpret_cast<BlockHeader*>(target) - 1;
        god_reg_t old_tag = hdr->god_tag.load(std::memory_order_relaxed);
        hdr->god_tag.store(set_worker_and_alloc(old_tag, t_cache.worker_id, true), std::memory_order_release);
        return reinterpret_cast<void*>(target);
    }

    // 3. Process Remote Mailbox
    if (t_cache.worker->remote_free_head.load(std::memory_order_relaxed) != 0) {
        drain_remote_mailbox(t_cache.worker);
        if (t_cache.count[bin] > 0) {
            t_cache.count[bin]--;
            uintptr_t target = t_cache.free_stack[bin][t_cache.count[bin]];
            BlockHeader* hdr = reinterpret_cast<BlockHeader*>(target) - 1;
            god_reg_t old_tag = hdr->god_tag.load(std::memory_order_relaxed);
            hdr->god_tag.store(set_worker_and_alloc(old_tag, t_cache.worker_id, true), std::memory_order_release);
            return reinterpret_cast<void*>(target);
        }
    }

    // 4. Refill from Spans (Loads L0 Bitmap Slab or L1 Stack)
    refill_tcache_from_spans(bin);

    bmp = t_cache.slab_bitmap[bin];
    if (__builtin_expect(bmp != ~0ULL, 1)) {
        uint32_t idx = __builtin_ctzll(~bmp);
        t_cache.slab_bitmap[bin] = bmp | (1ULL << idx);
        size_t stride = sizeof(BlockHeader) + class_to_size(bin);
        uintptr_t user_addr = t_cache.slab_base[bin] + (idx * stride);
        BlockHeader* hdr = reinterpret_cast<BlockHeader*>(user_addr) - 1;
        god_reg_t old_tag = hdr->god_tag.load(std::memory_order_relaxed);
        hdr->god_tag.store(set_worker_and_alloc(old_tag, t_cache.worker_id, true), std::memory_order_release);
        return reinterpret_cast<void*>(user_addr);
    }

    if (t_cache.count[bin] > 0) {
        t_cache.count[bin]--;
        uintptr_t target = t_cache.free_stack[bin][t_cache.count[bin]];
        BlockHeader* hdr = reinterpret_cast<BlockHeader*>(target) - 1;
        god_reg_t old_tag = hdr->god_tag.load(std::memory_order_relaxed);
        hdr->god_tag.store(set_worker_and_alloc(old_tag, t_cache.worker_id, true), std::memory_order_release);
        return reinterpret_cast<void*>(target);
    }

    return nullptr;
}

// Compile-time constant stride macro bounded by exact slab_count[bin] to prevent cross-span aliasing
#define EFLASH_FAST_L0_CHECK(STRIDE_CONST) \
    do { \
        if (__builtin_expect(slab_base != 0 && diff < (static_cast<size_t>(t_cache.slab_count[bin]) * (STRIDE_CONST)), 1)) { \
            if (__builtin_expect((diff % (STRIDE_CONST)) == 0, 1)) { \
                uint32_t idx = static_cast<uint32_t>(diff / (STRIDE_CONST)); \
                t_cache.slab_bitmap[bin] &= ~(1ULL << idx); \
                return; \
            } \
        } \
    } while(0)

FLASH_ALWAYS_INLINE void eflash_free_core(void* ptr) noexcept {
    if (__builtin_expect(!ptr, 0)) return;

    BlockHeader* hdr = static_cast<BlockHeader*>(ptr) - 1;
    god_reg_t tag = hdr->god_tag.fetch_and(~ALLOCATED_BIT, std::memory_order_acq_rel);
    if (__builtin_expect(!is_valid_allocated(tag), 0)) return;

    uint8_t bin = get_size_class_reg(tag);

    if (__builtin_expect(bin == FLASH_LARGE_BIN_TAG, 0)) {
        void* raw_mem = reinterpret_cast<void*>(hdr->parent_span);
        if (__builtin_expect(!raw_mem, 0)) return;
        hdr->parent_span = nullptr;
        hdr->god_tag.store(0ULL, std::memory_order_release);
        size_t total_bytes = static_cast<size_t>(get_offset(tag)) << 12;
        if (!push_large_extent_cache(raw_mem, total_bytes)) {
            os_page_free(raw_mem, total_bytes);
        }
        return;
    }

    if (__builtin_expect(bin >= FLASH_NUM_BINS, 0)) return;

    uintptr_t user_addr = reinterpret_cast<uintptr_t>(ptr);
    uint16_t  owner     = get_worker(tag);

    if (__builtin_expect(t_cache.torn_down || EFlash_State.load(std::memory_order_acquire) != 2, 0)) {
        if (G_Zone) return_block_to_span_lockfree(user_addr, hdr);
        return;
    }
    if (__builtin_expect(!t_cache.initialized, 0)) init_thread_worker();

    if (__builtin_expect(owner == t_cache.worker_id, 1)) {
        // 1. ATTEMPT L0 BITMAP SLAB RETURN (Zero-Division Fast Path)
        uintptr_t slab_base = t_cache.slab_base[bin];
        size_t diff = user_addr - slab_base;

        switch (bin) {
            case 0:  EFLASH_FAST_L0_CHECK(32ULL);     break;
            case 1:  EFLASH_FAST_L0_CHECK(48ULL);     break;
            case 2:  EFLASH_FAST_L0_CHECK(80ULL);     break;
            case 3:  EFLASH_FAST_L0_CHECK(144ULL);    break;
            case 4:  EFLASH_FAST_L0_CHECK(272ULL);    break;
            case 5:  EFLASH_FAST_L0_CHECK(528ULL);    break;
            case 6:  EFLASH_FAST_L0_CHECK(1040ULL);   break;
            case 7:  EFLASH_FAST_L0_CHECK(2064ULL);   break;
            case 8:  EFLASH_FAST_L0_CHECK(4112ULL);   break;
            case 9:  EFLASH_FAST_L0_CHECK(8208ULL);   break;
            case 10: EFLASH_FAST_L0_CHECK(16400ULL);  break;
            case 11: EFLASH_FAST_L0_CHECK(32784ULL);  break;
            case 12: EFLASH_FAST_L0_CHECK(65552ULL);  break;
            case 13: EFLASH_FAST_L0_CHECK(131088ULL); break;
            case 14: EFLASH_FAST_L0_CHECK(262160ULL); break;
            default: break;
        }

        // 2. FALLBACK TO L1 ARRAY STACK
        t_cache.free_stack[bin][t_cache.count[bin]++] = user_addr;
        if (__builtin_expect(t_cache.count[bin] >= FLASH_TCACHE_MAX, 0)) {
            drain_tcache_bin(bin, FLASH_TCACHE_BATCH);
        }
        return;
    }

    uint16_t safe_owner = static_cast<uint16_t>(owner % FLASH_MAX_WORKERS);
    if (__builtin_expect(t_cache.batch_count > 0 && t_cache.batch_owner != safe_owner, 0)) flush_remote_batch();

    uint64_t* payload = static_cast<uint64_t*>(ptr);
    if (t_cache.batch_count == 0) {
        t_cache.batch_owner = safe_owner;
        t_cache.batch_head  = user_addr;
        t_cache.batch_tail  = user_addr;
        *payload            = G_Zone->canary_seed;
        t_cache.batch_count = 1;
    } else {
        *payload            = t_cache.batch_head ^ G_Zone->canary_seed;
        t_cache.batch_head  = user_addr;
        t_cache.batch_count++;
    }

    // Flush promptly for medium blocks (>= 16KB) or when bounded batch reaches 64
    if (__builtin_expect(t_cache.batch_count >= FLASH_REMOTE_BATCH_MAX || bin >= 10, 0)) {
        flush_remote_batch();
    }
}

#undef EFLASH_FAST_L0_CHECK

FLASH_ALWAYS_INLINE size_t eflash_usable_size(void* ptr) noexcept {
    if (!ptr) return 0;
    BlockHeader* hdr = static_cast<BlockHeader*>(ptr) - 1;
    god_reg_t tag    = hdr->god_tag.load(std::memory_order_acquire);
    if (!is_valid_allocated(tag)) return 0;

    uint8_t bin = get_size_class_reg(tag);
    if (bin == FLASH_LARGE_BIN_TAG) {
        size_t total_bytes = static_cast<size_t>(get_offset(tag)) << 12;
        uintptr_t raw_base = reinterpret_cast<uintptr_t>(hdr->parent_span);
        uintptr_t user_ptr = reinterpret_cast<uintptr_t>(ptr);
        return (raw_base + total_bytes) - user_ptr;
    }
    if (bin >= FLASH_NUM_BINS) return 0;
    return class_to_size(bin);
}

#endif // FLASH_H
