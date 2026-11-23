// ============================================================================
// ANANTH E-FLASH V2.0 - REPRODUCIBLE MULTI-WORKLOAD BENCHMARK SUITE
// Compares Small (Tier 1), Medium (Tier 2), Large (Tier 3), and Multi-Threaded
// ============================================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static inline void escape(void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

static double run_single_size_bench(size_t sz, size_t iters, size_t batch) {
    std::vector<void*> ptrs(batch, nullptr);
    auto t0 = Clock::now();
    for (size_t r = 0; r < iters / batch; ++r) {
        for (size_t i = 0; i < batch; ++i) {
            void* p = std::malloc(sz);
            static_cast<volatile char*>(p)[0] = 1;
            escape(p);
            ptrs[i] = p;
        }
        for (size_t i = 0; i < batch; ++i) {
            std::free(ptrs[i]);
        }
    }
    auto t1 = Clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    return (static_cast<double>(iters) / sec) / 1e6; // M ops/sec (alloc+free pairs)
}

static double run_multithread_bench(int nthreads, size_t sz, size_t iters_per_thread) {
    constexpr size_t BATCH = 64;
    auto t0 = Clock::now();
    std::vector<std::thread> ts;
    for (int t = 0; t < nthreads; ++t) {
        ts.emplace_back([sz, iters_per_thread]() {
            std::vector<void*> ptrs(BATCH, nullptr);
            for (size_t r = 0; r < iters_per_thread / BATCH; ++r) {
                for (size_t i = 0; i < BATCH; ++i) {
                    void* p = std::malloc(sz);
                    static_cast<volatile char*>(p)[0] = 1;
                    escape(p);
                    ptrs[i] = p;
                }
                for (size_t i = 0; i < BATCH; ++i) {
                    std::free(ptrs[i]);
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    auto t1 = Clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    return (static_cast<double>(iters_per_thread * nthreads) / sec) / 1e6;
}

int main(int argc, char** argv) {
    const char* label = (argc > 1) ? argv[1] : "allocator";

    double small_64b   = run_single_size_bench(64, 2000000, 64);
    double med_64kb    = run_single_size_bench(65536, 40000, 16);
    double med_256kb   = run_single_size_bench(262144, 10000, 8);
    double large_1mb   = run_single_size_bench(1048576, 4000, 4);
    double mt_4t_64b   = run_multithread_bench(4, 64, 1000000);
    double mt_4t_64kb  = run_multithread_bench(4, 65536, 10000);

    std::printf("[%s] Tier1 64B ST:     %8.2f M alloc+free/sec\n", label, small_64b);
    std::printf("[%s] Tier2 64KB ST:    %8.2f M alloc+free/sec\n", label, med_64kb);
    std::printf("[%s] Tier2 256KB ST:   %8.2f M alloc+free/sec\n", label, med_256kb);
    std::printf("[%s] Tier3 1MB ST:     %8.2f M alloc+free/sec\n", label, large_1mb);
    std::printf("[%s] Tier1 64B 4T:     %8.2f M alloc+free/sec\n", label, mt_4t_64b);
    std::printf("[%s] Tier2 64KB 4T:    %8.2f M alloc+free/sec\n", label, mt_4t_64kb);
    return 0;
}
