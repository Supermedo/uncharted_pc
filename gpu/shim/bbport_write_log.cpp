// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_write_log.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#undef near
#else
#include <ucontext.h>
#include <unistd.h>
#endif
#include <x86intrin.h>

namespace VideoCore {
void PrintFaultHistory(unsigned long long address, unsigned long long size);
}

namespace BbWriteLog {
namespace {
struct Entry {
    std::uint64_t address, size, first, tsc;
    std::uint32_t source, tid;
};
constexpr std::size_t Size = 1 << 20;
std::array<Entry, Size> ring;
std::atomic<std::uint64_t> head{0};
// Writes that contained the suspicious qword (see Note), with the exact address.
std::array<Entry, 256> hits;
std::atomic<std::uint64_t> hits_head{0};
constexpr std::uint64_t Pattern = 0x0000005300000000ull;

void Push(std::array<Entry, Size>& r, std::atomic<std::uint64_t>& h, const Entry& e) {
    r[h.fetch_add(1, std::memory_order_relaxed) % r.size()] = e;
}
} // namespace

void Record(std::uint64_t address, const void* data, std::uint64_t size, Source source);

int Mode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_WRITE_LOG");
        return env ? std::atoi(env) : 0;
    }();
    return mode;
}

bool Enabled() {
    return Mode() == 1;
}

void NoteIntent(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    if (Mode() == 2) {
        Record(address, data, size, source);
    }
}

void Note(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    if (Enabled()) {
        Record(address, data, size, source);
    }
}

void Record(std::uint64_t address, const void* data, std::uint64_t size, Source source) {
    static thread_local const std::uint32_t tid = static_cast<std::uint32_t>(gettid());
    Entry e{address, size, 0, __rdtsc(), source, tid};
    std::memcpy(&e.first, data, size < 8 ? size : 8);
    Push(ring, head, e);
    // Only small writes are scanned: scanning downloads of megabytes delays them enough to hide
    // the race this log is for.
    if (size > 64) {
        return;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::uint64_t at = (8 - (address & 7)) & 7; at + 8 <= size; at += 8) {
        std::uint64_t v;
        std::memcpy(&v, bytes + at, 8);
        if (v == Pattern) {
            Entry hit = e;
            hit.address = address + at;
            hit.first = v;
            hits[hits_head.fetch_add(1, std::memory_order_relaxed) % hits.size()] = hit;
        }
    }
}
} // namespace BbWriteLog

extern "C" void bbgpu_dump_guest_writes(void* ucontext) {
    using namespace BbWriteLog;
    if (Mode() == 0) {
        return;
    }
#ifdef _WIN32
    // The loader passes EXCEPTION_POINTERS (see signal_context.cpp).
    const CONTEXT* c = static_cast<const EXCEPTION_POINTERS*>(ucontext)->ContextRecord;
    const std::uint64_t regs[] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R14, c->R15};
    std::fprintf(stderr, "Write log: r8=%#llx r9=%#llx r10=%#llx r11=%#llx r12=%#llx r13=%#llx\n",
                 (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->R10,
                 (unsigned long long)c->R11, (unsigned long long)c->R12, (unsigned long long)c->R13);
#else
    const auto* uc = static_cast<const ucontext_t*>(ucontext);
    const auto* g = uc->uc_mcontext.gregs;
    const std::uint64_t regs[] = {std::uint64_t(g[REG_RAX]), std::uint64_t(g[REG_RBX]),
                                  std::uint64_t(g[REG_RCX]), std::uint64_t(g[REG_RDX]),
                                  std::uint64_t(g[REG_RSI]), std::uint64_t(g[REG_RDI]),
                                  std::uint64_t(g[REG_R14]), std::uint64_t(g[REG_R15])};
#endif
    const char* names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r14", "r15"};
    for (int i = 0; i < 8; ++i) {
        std::fprintf(stderr, "Write log: %s=%#llx\n", names[i], (unsigned long long)regs[i]);
    }
    const char* sources[] = {"backing",      "WriteData",           "fence",
                             "EOP (decoded)", "WriteData (decoded)", "EOS (decoded)"};
    const std::uint64_t now = __rdtsc();
    const auto print = [&](const Entry& e, const char* what) {
        std::fprintf(stderr,
                     "Write log: %s %s %#llx +%llu first %#llx tid %u, %.3f s before the fault\n",
                     what, e.source < 6 ? sources[e.source] : "?", (unsigned long long)e.address,
                     (unsigned long long)e.size, (unsigned long long)e.first, e.tid,
                     double(now - e.tsc) / 3.0e9);
    };
    const std::uint64_t nh = hits_head.load();
    for (std::uint64_t i = nh > hits.size() ? nh - hits.size() : 0; i < nh; ++i) {
        print(hits[i % hits.size()], "pattern");
    }
    // BB_WRITE_LOG_HEAP=1: the guest faulted walking a free list of the game's heap allocator
    // (heap object in r15, head at +0x10, next pointer at node+0x10, magic at node+4). Walk it,
    // and take the last node with a readable pointer as the block of interest.
    std::uint64_t block = regs[0];
    if (std::getenv("BB_WRITE_LOG_HEAP")) {
        const auto readable = [](std::uint64_t a, std::uint64_t n) {
#ifdef _WIN32
            return a < 0x0000800000000000ull && !IsBadReadPtr(reinterpret_cast<const void*>(a), n);
#else
            return a < 0x0000800000000000ull && a > 0x1000;
#endif
        };
        std::uint64_t node = 0, prev = 0;
        if (readable(regs[7] + 0x10, 8)) {
            std::memcpy(&node, reinterpret_cast<const void*>(regs[7] + 0x10), 8);
        }
        for (int i = 0; i < 100000 && readable(node, 0x18); ++i) {
            std::uint32_t size_or_magic;
            std::uint64_t next;
            std::memcpy(&size_or_magic, reinterpret_cast<const void*>(node + 4), 4);
            std::memcpy(&next, reinterpret_cast<const void*>(node + 0x10), 8);
            std::fprintf(stderr, "Heap walk: node %#llx +4=%#x next %#llx\n", (unsigned long long)node,
                         size_or_magic, (unsigned long long)next);
            prev = node;
            node = next;
        }
        if (prev) {
            block = prev;
            std::fprintf(stderr, "Heap walk: last valid node %#llx, then %#llx\n", (unsigned long long)prev,
                         (unsigned long long)node);
        }
    }
    if (std::getenv("BB_WRITE_LOG_HEAP")) {
        VideoCore::PrintFaultHistory((block & ~0xfffull) - 0x1000, 0x3000);
    }
    for (std::uint64_t at = block - 0x30; at < block + 0x80; at += 8) {
        std::uint64_t value = 0;
        std::memcpy(&value, reinterpret_cast<const void*>(at), 8);
        std::fprintf(stderr, "Write log: [%#llx] = %#llx\n", (unsigned long long)at,
                     (unsigned long long)value);
    }
    {
        const std::uint64_t n = head.load();
        int shown = 0;
        for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 200;) {
            const Entry& e = ring[i % Size];
            if (e.address + e.size > block - 0x30 && e.address < block + 0x60) {
                print(e, "in block");
                ++shown;
            }
        }
        shown = 0;
        for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 40;) {
            const Entry& e = ring[i % Size];
            if ((e.first & 0xffffffffull) == 0x53 || (e.first >> 32) == 0x53) {
                print(e, "value 0x53");
                ++shown;
            }
        }
    }
    // Writes that cover the chunk header the guest read (rax..rax+0x40) or the registers.
    const std::uint64_t n = head.load();
    int shown = 0;
    for (std::uint64_t i = n; i-- > (n > Size ? n - Size : 0) && shown < 64;) {
        const Entry& e = ring[i % Size];
        bool near = false;
        for (const auto r : regs) {
            near |= r + 0x1000 > e.address && r < e.address + e.size + 0x1000;
        }
        if (near) {
            print(e, "near");
            ++shown;
        }
    }
    std::fprintf(stderr, "Write log: %llu writes logged\n", (unsigned long long)n);
}
