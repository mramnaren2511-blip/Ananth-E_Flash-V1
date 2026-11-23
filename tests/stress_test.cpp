// ============================================================================
// ANANTH E-FLASH V2.0 - CORRECTNESS, EDGE-CASE & CONCURRENCY STRESS SUITE
// ============================================================================
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>

static void check_pattern(const uint8_t* p, size_t n, uint8_t val) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != val) {
            std::fprintf(stderr, "FAIL: pattern mismatch at byte %zu (got %u, expected %u)\n", i, p[i], val);
            std::abort();
        }
    }
}

static void test_all_size_tiers() {
    const size_t sizes[] = {
        0, 1, 16, 24, 32, 64, 128, 256, 512, 1024, 4096, 8192, 16384, 32768, // Tier 1
        49152, 65536, 131072, 262144,                                         // Tier 2 (2MB Sub-Spans)
        393216, 524288, 1048576, 2097152, 4194304,                            // Tier 3 (Large Cache)
        6291456, 8388608                                                      // Tier 4 (Direct OS)
    };

    for (size_t sz : sizes) {
        uint8_t* p = static_cast<uint8_t*>(std::malloc(sz));
        assert(p != nullptr);
        assert((reinterpret_cast<uintptr_t>(p) & 15) == 0);
        if (sz > 0) {
            size_t usable = malloc_usable_size(p);
            assert(usable >= sz);
            uint8_t pat = static_cast<uint8_t>((sz * 37 + 11) & 0xFF);
            std::memset(p, pat, sz);
            check_pattern(p, sz, pat);
        }
        std::free(p);
    }
    std::printf("[PASS] test_all_size_tiers (Tier 1..4: 0B to 8MB)\n");
}

static void test_aligned_and_overflow_guards() {
    const size_t aligns[] = {16, 32, 64, 256, 4096, 8192, 65536};
    std::vector<void*> ptrs;
    for (size_t al : aligns) {
        for (int i = 0; i < 16; ++i) {
            void* p = nullptr;
            int rc = posix_memalign(&p, al, al * 2);
            assert(rc == 0 && p != nullptr);
            assert((reinterpret_cast<uintptr_t>(p) % al) == 0);
            std::memset(p, 0x5A, al * 2);
            ptrs.push_back(p);
        }
    }
    for (void* p : ptrs) std::free(p);

    // Overflow / huge-alignment guard check (must return nullptr / ENOMEM cleanly)
    void* bad1 = aligned_alloc(4096, SIZE_MAX - 1024);
    assert(bad1 == nullptr);
    void* bad2 = nullptr;
    int rc2 = posix_memalign(&bad2, (1ULL << 62), 64);
    assert(rc2 == ENOMEM && bad2 == nullptr);
    void* bad3 = std::calloc(SIZE_MAX / 2, 4);
    assert(bad3 == nullptr);

    std::printf("[PASS] test_aligned_and_overflow_guards\n");
}

static void test_calloc_zeroing_all_tiers() {
    // Dirty Tier 1, Tier 2, and Tier 3 blocks, free them, then calloc to ensure zero-fill
    const size_t sizes[] = {64, 4096, 65536, 262144, 524288, 1048576, 2097152};
    for (size_t sz : sizes) {
        void* dirty = std::malloc(sz);
        assert(dirty != nullptr);
        std::memset(dirty, 0xCC, sz);
        std::free(dirty);

        uint8_t* z = static_cast<uint8_t*>(std::calloc(1, sz));
        assert(z != nullptr);
        check_pattern(z, sz, 0x00);
        std::free(z);
    }
    std::printf("[PASS] test_calloc_zeroing_all_tiers (Tier 1, 2 & 3 cached reuse)\n");
}

static void test_realloc_integrity() {
    uint8_t* p = static_cast<uint8_t*>(std::malloc(64));
    assert(p != nullptr);
    for (size_t i = 0; i < 64; ++i) p[i] = static_cast<uint8_t>(i);

    const size_t chain[] = {256, 4096, 65536, 524288, 1048576, 128, 32};
    size_t prev_sz = 64;
    for (size_t next_sz : chain) {
        p = static_cast<uint8_t*>(std::realloc(p, next_sz));
        assert(p != nullptr);
        size_t check_n = (prev_sz < next_sz) ? prev_sz : next_sz;
        for (size_t i = 0; i < check_n; ++i) {
            assert(p[i] == static_cast<uint8_t>(i & 0xFF));
        }
        for (size_t i = check_n; i < next_sz; ++i) {
            p[i] = static_cast<uint8_t>(i & 0xFF);
        }
        prev_sz = next_sz;
    }
    std::free(p);
    std::printf("[PASS] test_realloc_integrity (grow & shrink across all tiers)\n");
}

static void test_multithreaded_cross_free_and_registry() {
    constexpr int NUM_PAIRS = 4;
    constexpr int ITERS = 5000;

    std::vector<std::thread> threads;
    for (int pair = 0; pair < NUM_PAIRS; ++pair) {
        auto queue = new std::vector<void*>();
        queue->reserve(ITERS);
        auto mtx = new std::mutex();
        auto done = new std::atomic<bool>(false);

        threads.emplace_back([pair, queue, mtx, done]() {
            for (int i = 0; i < ITERS; ++i) {
                void* p = nullptr;
                if (i % 17 == 0) {
                    // Concurrent page-aligned registry insertion stress
                    int rc = posix_memalign(&p, 4096, 4096);
                    assert(rc == 0 && p != nullptr);
                    static_cast<uint8_t*>(p)[0] = static_cast<uint8_t>(pair);
                } else {
                    size_t sz = (i % 4 == 0) ? 65536 : ((i % 3 == 0) ? 1024 : 64);
                    p = std::malloc(sz);
                    assert(p != nullptr);
                    static_cast<uint8_t*>(p)[0] = static_cast<uint8_t>(pair);
                }
                std::lock_guard<std::mutex> lk(*mtx);
                queue->push_back(p);
            }
            done->store(true, std::memory_order_release);
        });

        threads.emplace_back([pair, queue, mtx, done]() {
            int freed = 0;
            while (freed < ITERS) {
                void* p = nullptr;
                {
                    std::lock_guard<std::mutex> lk(*mtx);
                    if (!queue->empty()) {
                        p = queue->back();
                        queue->pop_back();
                    }
                }
                if (p) {
                    assert(static_cast<uint8_t*>(p)[0] == static_cast<uint8_t>(pair));
                    std::free(p);
                    freed++;
                } else if (done->load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
            delete queue;
            delete mtx;
            delete done;
        });
    }

    for (auto& t : threads) t.join();
    std::printf("[PASS] test_multithreaded_cross_free_and_registry (8 threads, 20K cross-frees + page registry)\n");
}

static void test_thread_churn() {
    for (int round = 0; round < 32; ++round) {
        std::thread t([round]() {
            std::vector<void*> local;
            for (int i = 0; i < 200; ++i) {
                size_t sz = (i % 10 == 0) ? 131072 : 128;
                void* p = std::malloc(sz);
                assert(p != nullptr);
                static_cast<uint8_t*>(p)[0] = static_cast<uint8_t>(round);
                local.push_back(p);
            }
            for (void* p : local) std::free(p);
        });
        t.join();
    }
    std::printf("[PASS] test_thread_churn (32 thread create/teardown cycles)\n");
}

int main() {
    test_all_size_tiers();
    test_aligned_and_overflow_guards();
    test_calloc_zeroing_all_tiers();
    test_realloc_integrity();
    test_multithreaded_cross_free_and_registry();
    test_thread_churn();
    std::printf("ALL ANANTH E-FLASH V2.0 TESTS PASSED!\n");
    return 0;
}
