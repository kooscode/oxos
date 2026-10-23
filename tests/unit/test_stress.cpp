// Stress under asynchronous interrupts: the clock interrupt lands on any kernel instruction (as the PIT does on a
// console), while threads hammer the pool, system memory, contiguous memory (page moves), thread creation and waits,
// and check every byte they wrote. Races that synchronous delivery never exposes show up here as corruption, a kernel
// page fault, a bug check or a deadlock. OXOS_STRESS_SECONDS sets the length (default 5 s of real time).
#include "kernel_test.h"

#include <atomic>
#include <cstdlib>
#include <ctime>

extern volatile ULONG ExpPoolBlocks;

namespace {
std::atomic<bool> Stop;
std::atomic<unsigned long> Rounds, Errors, ShortThreads;

uint32_t Next(uint32_t &x) {
    x = x * 1664525 + 1013904223;
    return x >> 8;
}
void Fill(uint32_t *p, size_t words, uint32_t seed) {
    for (size_t i = 0; i < words; i++)
        p[i] = seed ^ (uint32_t)(i * 0x9E3779B9u);
}
bool Check(const uint32_t *p, size_t words, uint32_t seed) {
    for (size_t i = 0; i < words; i++)
        if (p[i] != (seed ^ (uint32_t)(i * 0x9E3779B9u))) {
            Errors++;
            return false;
        }
    return true;
}

void Load(unsigned id, unsigned loads) {
    uint32_t x = id * 7919 + 1;
    void *blocks[64] = {};
    size_t sizes[64] = {};
    while (!Stop) {
        if (loads & 1)
            for (int k = 0; k < 64; k++) {
                unsigned i = Next(x) % 64;
                if (blocks[i]) {
                    Check((uint32_t *)blocks[i], sizes[i] / 4, (uint32_t)(uintptr_t)blocks[i]);
                    ExFreePool(blocks[i]);
                }
                sizes[i] = (4 + Next(x) % (Next(x) % 4 ? 1000 : 9000)) & ~3u;
                blocks[i] = ExAllocatePoolWithTag(sizes[i], 'srtS');
                if (blocks[i])
                    Fill((uint32_t *)blocks[i], sizes[i] / 4, (uint32_t)(uintptr_t)blocks[i]);
            }
        if (loads & 2) {
            ULONG bytes = (1 + Next(x) % 16) * PAGE_SIZE;
            if (auto s = (uint32_t *)MmAllocateSystemMemory(bytes, PAGE_READWRITE)) {
                Fill(s, bytes / 4, bytes);
                Check(s, bytes / 4, bytes);
                MmFreeSystemMemory(s, bytes);
            }
        }
        if (loads & 4) {
            ULONG bytes = (1 + Next(x) % 32) * PAGE_SIZE;
            if (auto c = (uint32_t *)MmAllocateContiguousMemory(bytes)) {
                Fill(c, bytes / 4, 0xC0C0 ^ bytes);
                Check(c, bytes / 4, 0xC0C0 ^ bytes);
                MmFreeContiguousMemory(c);
            }
        }
        if ((loads & 8) && Next(x) % 8 == 0)
            Join(Spawn([] { ShortThreads++; }));
        if (Next(x) % 4 == 0)
            SleepMs(Next(x) % 3);
        Rounds++;
    }
    for (void *b : blocks)
        if (b)
            ExFreePool(b);
}

void RunStress(unsigned loads, const char *what) {
    const char *env = getenv("OXOS_STRESS_SECONDS");
    double seconds = env && *env ? atof(env) : 5;
    Stop = false;
    Rounds = Errors = ShortThreads = 0;
    ULONG available = MmAvailablePages, blocks = ExpPoolBlocks;
    uint64_t delivered = host::AsyncDelivered, pended = host::AsyncPended;
    PKTHREAD a = Spawn([=] { Load(1, loads); }, 8), b = Spawn([=] { Load(2, loads); }, 8);
    host::SetAsyncInterrupts(50);
    timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        host::IdleTicksThisTest = 0; // the deadlock guard counts idle time; this loop is the proof of progress
        SleepMs(10);
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 < seconds);
    Stop = true;
    Join(a);
    Join(b);
    host::SetAsyncInterrupts(0);
    printf("  [%s] %lu rounds, %lu short threads; clock interrupts at a kernel instruction %llu, left pending %llu\n",
           what, Rounds.load(), ShortThreads.load(), (unsigned long long)(host::AsyncDelivered - delivered),
           (unsigned long long)(host::AsyncPended - pended));
    EXPECT_EQ(Errors.load(), 0u) << "memory written by the load came back different";
    EXPECT_GT(Rounds.load(), 0u);
    EXPECT_GT(host::AsyncDelivered - delivered, 0u) << "no interrupt landed inside kernel code";
    EXPECT_EQ(ExpPoolBlocks, blocks);
    if (!(loads & 1)) // the pool keeps up to two freed blocks per small size (lookaside), and so their pages
        EXPECT_EQ(MmAvailablePages, available);
    else
        EXPECT_GE(MmAvailablePages + 16, available);
    EXPECT_EQ(CheckPfnDatabase(), "");
}
} // namespace

TEST(Stress, PoolUnderAsyncInterrupts) { RunStress(1, "pool"); }
TEST(Stress, SystemMemoryUnderAsyncInterrupts) { RunStress(2, "system memory"); }
TEST(Stress, ContiguousMemoryUnderAsyncInterrupts) { RunStress(4, "contiguous memory"); }
TEST(Stress, ThreadsUnderAsyncInterrupts) { RunStress(8, "threads"); }
TEST(Stress, EverythingUnderAsyncInterrupts) { RunStress(15, "everything"); }
