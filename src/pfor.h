#pragma once

// Minimal parallel-for over a row range using OS threads. Every invocation
// splits [begin, end) into per-core chunks; results are identical to a serial
// loop as long as the body only touches disjoint output rows.
// std::thread workers (C++11, no extra runtime dependency).

#include <algorithm>
#include <system_error>
#include <thread>
#include <vector>

template <typename F>
inline void parallelFor(int begin, int end, const F &fn)
{
    const int rows = end - begin;
    if (rows < 64) {                     // too small to pay for threads
        for (int i = begin; i < end; i++)
            fn(i);
        return;
    }
    unsigned nt = std::thread::hardware_concurrency();
    if (nt <= 1) {
        for (int i = begin; i < end; i++)
            fn(i);
        return;
    }
    nt = std::min(nt, (unsigned)rows);
    const int chunk = (rows + int(nt) - 1) / int(nt);

    std::vector<std::thread> threads;
    threads.reserve(nt);

    int b = begin;
    while (b < end) {
        const int e = std::min(end, b + chunk);
        try {
            threads.emplace_back([&fn, b, e] {
                for (int i = b; i < e; i++)
                    fn(i);
            });
        } catch (const std::system_error &) {   // thread creation failed: run inline
            for (int i = b; i < e; i++)
                fn(i);
        }
        b = e;
    }
    for (auto &t : threads)
        t.join();
}