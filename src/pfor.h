#pragma once

// Minimal parallel-for over a row range using OS threads. Every invocation
// splits [begin, end) into per-core chunks; results are identical to a serial
// loop as long as the body only touches disjoint output rows.
// Windows: CreateThread workers (no extra runtime dependency). Elsewhere:
// serial fallback.

#include <algorithm>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

template <typename F>
struct PforArgs {
    const F *fn;
    int b, e;
};

template <typename F>
static DWORD WINAPI pforWorker(LPVOID p)
{
    PforArgs<F> *a = static_cast<PforArgs<F> *>(p);
    for (int i = a->b; i < a->e; i++)
        (*a->fn)(i);
    return 0;
}

template <typename F>
inline void parallelFor(int begin, int end, const F &fn)
{
    const int rows = end - begin;
    if (rows < 64) {                     // too small to pay for threads
        for (int i = begin; i < end; i++)
            fn(i);
        return;
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned nt = si.dwNumberOfProcessors;
    if (nt <= 1) {
        for (int i = begin; i < end; i++)
            fn(i);
        return;
    }
    nt = std::min(nt, (unsigned)rows);
    const int chunk = (rows + int(nt) - 1) / int(nt);

    std::vector<PforArgs<F>> args;
    std::vector<HANDLE> handles;
    args.reserve(nt);
    handles.reserve(nt);

    int b = begin;
    for (unsigned t = 0; t < nt; t++) {
        const int e = std::min(end, b + chunk);
        if (b >= e)
            break;
        args.push_back({&fn, b, e});
        HANDLE h = CreateThread(nullptr, 0, &pforWorker<F>, &args.back(), 0, nullptr);
        if (h) {
            handles.push_back(h);
        } else {                          // thread creation failed: run inline
            for (int i = b; i < e; i++)
                fn(i);
        }
        b = e;
    }
    for (HANDLE h : handles) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
}

#else

template <typename F>
inline void parallelFor(int begin, int end, const F &fn)
{
    for (int i = begin; i < end; i++)
        fn(i);
}

#endif