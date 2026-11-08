# ⚡ Ananth E-Flash V1.0
**Ultra-Low-Latency, Lock-Free, OS-Agnostic C++ Memory Allocator**

[![License: Ananth Fair-Use](https://img.shields.io/badge/License-Ananth_Fair--Use_(%3C$2M_Free)-blue.svg)](#license--commercial-terms)
[![Architecture: x86_64 | ARM64](https://img.shields.io/badge/Arch-x86__64_%7C_ARM64-success.svg)](#core-architecture)
[![Language: C++17/20](https://img.shields.io/badge/C%2B%2B-17%2F20-orange.svg)](#quick-start--integration)
[![Patent Pending](https://img.shields.io/badge/Patent_Pending-202641127187-critical.svg)](#license--commercial-terms)

**Ananth E-Flash V1.0** is a bare-metal, lock-free memory allocator engineered in C++ to eliminate allocation bottlenecks in ultra-low-latency environments such as High-Frequency Trading (HFT) systems, real-time game engines, and hyperscale cloud infrastructure.

---

## 🤝 A Humble Note from the Creator

Hello! I built **Ananth E-Flash** with a relentless obsession for bare-metal speed, lock-free concurrency, and zero-overhead memory management. 

However, **I do not claim this code to be 100% perfect.** Memory management at the OS and kernel boundary is unforgiving. While this engine has passed aggressive local stress tests, there may still be hidden edge cases, extreme multi-threading race conditions, ABA corner cases, or architecture-specific bugs that I have not caught yet.

I am opening up this source code because I believe true foundational technology is forged through peer review. **I humbly invite the global C++ community, senior systems engineers, HFT developers, and OS researchers to freely tear this code apart:**
* Stress-test it under your most brutal workloads.
* Point out my architectural mistakes or memory-ordering flaws.
* Submit Pull Requests, bug reports, and critiques to help rectify any issues.

You have a completely free hand to audit, critique, and improve this engine. Let's build the ultimate memory allocator together.

---

## 🧠 Core Architecture

Ananth E-Flash replaces traditional pointer-chasing freelists and heavy mutex locks with a hybrid hardware-accelerated hierarchy:

### 1. L0 64-Bit CTZLL Bitmap Slabs (Zero Pointer Chasing)
* Small allocations (`16B` to `32KB` across 12 size classes) are served directly from thread-local **64-block L0 Bitmap Slabs**.
* Allocation resolves in $O(1)$ time using a single hardware instruction (`__builtin_ctzll` / `tzcnt`) to locate the first available block without touching user payload memory.
* Deallocation on the local L0 slab uses compile-time constant stride multiplication instead of hardware division (`divq`), clearing the bit in a single CPU cycle.

### 2. L1 Fast Array Stack
* When an L0 slab is full or a freed pointer belongs to an older slab, blocks fall back to a thread-local **L1 Array Stack** (`FLASH_TCACHE_MAX = 256`).
* Unlike traditional intrusive linked lists that dereference freed memory (causing L1/L2 cache misses), the L1 Array Stack stores raw addresses contiguously in thread-local storage (TLS).

### 3. God Register V1.0 (64-Bit Packed Atomic Metadata)
Every block header packs its complete lifecycle state into a single 64-bit atomic word (`god_reg_t`), eliminating multi-field synchronization overhead:

| Bits | Field | Description |
| :--- | :--- | :--- |
| `0..37` (38 bits) | **Offset / Pages** | Exact byte offset within the 2MB Span (or page count for large allocations) |
| `38..49` (12 bits) | **Worker ID** | Owning thread/worker engine index (supports up to 1,024 concurrent workers) |
| `50..55` (6 bits) | **NUMA Node** | Hardware NUMA topology identifier |
| `56..61` (6 bits) | **Size Class** | Bin index (`0..11` for fast bins, `63` for direct OS huge allocations) |
| `62` (1 bit) | **Allocated Bit** | Atomic ownership flag (`1` = Allocated, `0` = Free) for double-free protection |
| `63` (1 bit) | **Magic Bit** | Hardware validation tag to distinguish E-Flash blocks from foreign pointers |

### 4. Lock-Free 2MB Hugepage Spans & XOR-Canary Remote Mailboxes
* Backing memory is mapped in **2MB Hugepage Spans** (`MADV_HUGEPAGE`) to minimize TLB misses.
* Central span freelists use **21-bit byte offsets paired with a 42-bit monotonic ABA tag** for lock-free CAS harvesting.
* Cross-thread `free()` calls are batched locally and flushed via lock-free atomic CAS to the owning worker's `remote_free_head`, protected against heap corruption via process-unique **XOR Canary Seeds**.

### 5. Zero-Recursion `LD_PRELOAD` Hijack Engine (`malloc_hijack.cpp`)
* **8MB Static Bootstrap Arena:** Safely intercepts early dynamic linker (`ld.so`), `dlsym`, and `pthread` TLS allocations before the main engine initializes, preventing infinite recursion.
* **Self-Healing Page-Boundary Registry:** Tracks explicit page-aligned allocations in a 256K-slot lock-free hash table so foreign OS pointers passed to `free()` or `realloc()` are safely ignored without segfaulting.

---

## 📊 Performance Benchmarks

In local stress testing on 64-bit Linux (`-O3 -march=native`), Ananth E-Flash demonstrated extreme throughput and stability:

* **5,000,000 Allocations & Deallocations:** Completed in **`0.333 seconds`** (~15M+ ops/sec single-thread baseline).
* **Multi-Threaded Contention:** Zero deadlocks, zero memory leaks, and deterministic latency scaling across worker threads.

*(Note: You can reproduce these benchmarks on your own hardware using your internal staging suite or standard allocator benchmarks like `mimalloc-bench`.)*

---

## 🚀 Quick Start & Integration

Ananth E-Flash has **zero external dependencies** (only standard `pthread` on Linux) and can be integrated in three ways:

### Option 1: Header-Only Core API (`flash.h`)
Include `flash.h` directly in your C++ project for raw access to the core engine:

```cpp
#include "flash.h"

int main() {
    void* ptr = eflash_malloc_core(128);
    // Use memory...
    eflash_free_core(ptr);
    return 0;
}
```

### Option 2: Direct Source Compilation (Drop-In Global Override)
Compile `malloc_hijack.cpp` alongside your application to automatically route all `malloc`, `free`, `calloc`, `realloc`, `new`, `delete`, `std::vector`, and `posix_memalign` calls through Ananth E-Flash:

```bash
g++ -O3 -march=native your_app.cpp malloc_hijack.cpp -lpthread -o your_app
./your_app
```

### Option 3: `LD_PRELOAD` Shared Library (Zero Recompilation)
Build the shared object `.so` file to accelerate existing binaries (C++ engines, Python, Redis, Node.js, etc.) without touching their source code:

```bash
# 1. Compile the shared library
g++ -O3 -march=native -shared -fPIC malloc_hijack.cpp -o libeflash.so -lpthread

# 2. Run any target binary with LD_PRELOAD
LD_PRELOAD=./libeflash.so ./your_high_frequency_app
```

---

## 🛠️ Contributing & Bug Reports

Found a bug, a race condition, a memory leak, or a way to shave off another CPU cycle? 
1. Fork the repository.
2. Create a branch (`git checkout -b fix/race-condition-in-span`).
3. Commit your changes with a clear explanation of the flaw you found and how you fixed it.
4. Open a **Pull Request** or create a detailed **GitHub Issue**.

Every critique and contribution is deeply appreciated!

---

## 📜 License & Commercial Terms

Ananth E-Flash is distributed under the **Ananth E-Tech Fair-Use License v1.0** (Patent Pending: `202641127187`).

### ✅ Free for Individuals, Researchers & Startups
You are granted a **free, perpetual license** to use, modify, and distribute this software (for both non-commercial and commercial products) if your organization meets **BOTH** of the following criteria:
1. **Less than $2,000,000 USD** in annual gross revenue.
2. **Less than $2,000,000 USD** in total cumulative raised funding.

### 🏢 Enterprise & Commercial Licensing
If your organization exceeds either the **$2,000,000 USD** revenue or funding threshold—including **High-Frequency Trading (HFT) firms, Quantitative Hedge Funds, Cloud Hyperscalers, Datacenters, and Large Enterprises**—you are **strictly prohibited** from using this software in any production, staging, testing, or internal environment without an active Commercial License.

* **Author:** Mane Ram Naren (Founder, Ananth E Tech)
* **Commercial Licensing & Enterprise Inquiries:** [ananthetech@gmail.com](mailto:ananthetech@gmail.com)

See the [LICENSE](./LICENSE) file for the complete legal terms and conditions.