# Ananth E-Flash V2.0

**Ultra-Low-Latency, Lock-Free Hybrid C++17 Memory Allocator (`LD_PRELOAD` Drop-In)**  
**Author:** Mane Ram Naren ([Ananth E Tech](https://github.com/mramnaren2511-blip/Ananth-E_Flash-V1))  
**Patent Pending:** Application No. `202641127187`  
**License:** Ananth E-Tech Fair-Use License (Free for individuals & startups under $2M revenue/funding; commercial license for enterprises)

---

## Hybrid 4-Tier Architecture

Ananth E-Flash V2.0 eliminates lock contention and kernel `mmap_lock` / TLB shootdown storms across small, medium, and large allocations:

| Tier | Allocation Range | Mechanism | Key Advantage |
| :--- | :--- | :--- | :--- |
| **Tier 1 (Fast Slabs)** | `16 B – 32 KB` (bins `0..11`) | L0 64-bit `__builtin_ctzll` bitmap slab + L1 TLS array stack | O(1) allocation, zero pointer chasing, compile-time stride arithmetic |
| **Tier 2 (Medium Spans)** | `64 KB – 256 KB` (bins `12..14`) | Sub-sliced inside 2 MB HugePage `CentralSpan`s | Eliminates `mmap`/`munmap` churn on network & engine buffers |
| **Tier 3 (Large Cache)** | `512 KB – 4 MB` | Lock-free bounded extent cache (`8` slots per power-of-two bucket) | Reuses large buffers without page faults or TLB shootdowns |
| **Tier 4 (Direct OS)** | `> 4 MB` (tag `63`) | Direct `mmap` + `MADV_HUGEPAGE` / `munmap` | Zero memory hoarding for multi-megabyte allocations |

### V2.0 Production Hardening Highlights
- **Race-Free Lock-Free Page Registry:** Removed unsafe concurrent tombstone-to-zero resets in `unregister_page_aligned_ptr`, preserving linear-probing chains under multi-threaded load.
- **Safe Foreign Pointer Bridging (`RTLD_NEXT`):** Delegates pre-`LD_PRELOAD` foreign `free`, `realloc`, and `malloc_usable_size` calls cleanly to the underlying libc allocator rather than truncating at 4 KB page boundaries.
- **ARM64 & Weak-Memory Safety:** Upgraded `eflash_min`/`eflash_max` bounds and `BlockHeader::god_tag` handoffs to release/acquire ordering.
- **Cross-Span Aliasing & Overflow Guards:** Bounded L0 slab returns by exact carved `slab_count[bin]`, tracked `os_page_alloc` early-bootstrap fallbacks, guarded huge alignments against unsigned underflow, and ensured `calloc` zeroes Tier 3 cached extents.
- **Bounded Remote-Free Batching:** Flushes remote mailboxes at `64` blocks (and immediately for `>= 16 KB` bins) to prevent Producer-Consumer RSS bloat.

---

## Quick Start (Build, Test & Benchmark)

```bash
# 1. Build libananth_eflash.so (-O3 -std=c++17)
make

# 2. Run correctness, edge-case, multi-threaded stress tests + real binary LD_PRELOAD checks
make test

# 3. Run reproducible side-by-side benchmark vs system glibc malloc
make bench
```

Use as a drop-in allocator for any Linux x86_64 or ARM64 binary:
```bash
LD_PRELOAD=./libananth_eflash.so ./your_application
```

---

## Verified Benchmark Results (Linux x86_64, GCC `-O3`, `LD_PRELOAD`)

Measured via `make bench` (`benchmarks/bench_suite.cpp`, throughput in million alloc+free pairs per second):

| Workload | System `glibc` | Ananth E-Flash V2.0 | Speedup |
| :--- | :---: | :---: | :---: |
| **Tier 2 — 64 KB (Single-Thread)** | 0.84 M/s | **34.84 M/s** | **41.5x** |
| **Tier 2 — 256 KB (Single-Thread)** | 0.52 M/s | **10.64 M/s** | **20.5x** |
| **Tier 3 — 1 MB (Single-Thread)** | 0.38 M/s | **2.42 M/s** | **6.4x** |
| **Tier 2 — 64 KB (4 Threads)** | 1.52 M/s | **7.94 M/s** | **5.2x** |
| **Tier 1 — 64 B (4 Threads)** | 68.72 M/s | **82.70 M/s** | **1.20x** |
| **Tier 1 — 64 B (Single-Thread)** | 85.76 M/s | 47.94 M/s | 0.56x* |

*\*Note on Single-Thread 64B: Under `LD_PRELOAD`, E-Flash V2.0 executes full MMU bounds verification and atomic `fetch_and` double-free protection on every call, whereas single-threaded glibc tcache skips atomic operations. Under multi-threaded load (4T) and medium/large buffers (64 KB – 1 MB), E-Flash V2.0 significantly outperforms glibc.*
