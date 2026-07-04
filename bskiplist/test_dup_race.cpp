#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "include/BSkipList.hpp"

constexpr int NUM_THREADS = 8;
constexpr int OPS_PER_THREAD = 2'000'000;
constexpr uint64_t KEY_SPACE = 100;

using Key = uint64_t;
using Val = uint64_t;

using traits = BSkip_traits<true, 128, 64, Key, Val>;
BSkip<traits> tree;

struct FastRand {
    uint64_t x;
    explicit FastRand(uint64_t seed) : x(seed ? seed : 1) {}
    inline uint64_t next() {
        x ^= x << 7;
        x ^= x >> 9;
        return x;
    }
};

void worker(int tid) {
    FastRand rng(tid * 1337 + 12345);
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        uint64_t key = rng.next() % KEY_SPACE + 1;
        uint64_t val = rng.next();
        tree.insert({key, val});
    }
}

int main() {
    printf("Duplicate-key race repro: %d threads x %d ops, key space=%lu\n",
           NUM_THREADS, OPS_PER_THREAD, KEY_SPACE);

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(worker, i);
    }
    for (auto &t : threads) t.join();

    printf("All threads joined without crashing/asserting. Validating structure...\n");
    tree.validate_structure();
    printf("validate_structure() passed.\n");
    return 0;
}
