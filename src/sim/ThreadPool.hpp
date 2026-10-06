// ThreadPool — a persistent worker pool for the beam's per-layer expansion.
//
// WHY: the beam is the solver's dominant cost and it is hard-capped on WALL-CLOCK time
// (Solver_Beam.cpp's kLimit). On "black off" (109927) it times out at 98% after 167s and
// the same beam solves the level at 329s given more clock — on a machine with 24 cores,
// 23 of which are idle. Every extra core is directly more search inside the same budget.
//
// WHY PERSISTENT: a hard level runs ~19 000 layers. Spawning threads per layer would cost
// far more than the ~14 ms of work each layer contains, so the threads are created once
// and parked on a condition variable between layers.
//
// DETERMINISM: parallelFor makes no ordering promise at all — it only guarantees that
// fn(i, worker) has run for every i in [0, count) when it returns. Callers that need a
// reproducible result must write into a PRE-SIZED slot indexed by i (never push_back into
// a shared container) and compact in index order afterwards. The beam does exactly that,
// so its output is bit-identical to the single-threaded run.
//
// Each task gets a `worker` id in [0, workers()) so callers can hand it a private
// scratch object — for the beam that is a private Level clone, which is mandatory:
// Level::stepPlayer poses movable objects in place and runFrame mutates gameStates, so
// two threads must never share one Level.
#pragma once
#include <atomic>
#include <cstdlib>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace gdsim {

class ThreadPool {
public:
    // n <= 0 picks hardware_concurrency (clamped to at least 1). n == 1 runs everything
    // inline on the calling thread with no threads created at all, which keeps the
    // single-threaded path exactly as it was — useful as an A/B reference.
    explicit ThreadPool(int n = 0) {
        if (n <= 0) n = (int)std::thread::hardware_concurrency();
        if (n <= 0) n = 1;
        m_workers = n;
        if (n == 1) return;
        m_threads.reserve(n - 1);
        for (int w = 1; w < n; ++w)
            m_threads.emplace_back([this, w] { loop(w); });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stop = true;
        }
        m_cvWork.notify_all();
        for (auto& t : m_threads) if (t.joinable()) t.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    int workers() const { return m_workers; }

    // Run fn(i, worker) for every i in [0, count). Blocks until all have completed.
    // The calling thread participates as worker 0, so a pool of N uses N threads total.
    // Below this many items a layer is not worth a wake-up + barrier round trip, so it
    // runs inline on the caller. MEASURED: without this guard the beam got SLOWER with
    // threads (24 threads 173s vs 1 thread 111s on truth 308) — a beam layer's frontier
    // starts tiny and stays small for long stretches, and paying ~23 thread wake-ups for
    // a handful of simulated frames costs far more than it saves.
    static int minParallel() {
        static const int v = [] {
            if (const char* s = getenv("GDSIM_MIN_PARALLEL")) { int x = atoi(s); if (x > 0) return x; }
            return 256;
        }();
        return v;
    }

    void parallelFor(int count, const std::function<void(int, int)>& fn) {
        if (count <= 0) return;
        if (m_workers == 1 || count < minParallel()) {  // inline: no locking, no wakeups
            for (int i = 0; i < count; ++i) fn(i, 0);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_fn    = &fn;
            m_count.store(count, std::memory_order_relaxed);
            m_next.store(0, std::memory_order_relaxed);
            m_done.store(0, std::memory_order_relaxed);
            ++m_generation;
        }
        m_cvWork.notify_all();
        run(0);                        // the caller pulls work too
        // Wait on ITEMS COMPLETED, never on "every worker has checked in".
        //
        // The first version counted workers: each parallelFor had to wait for all the
        // sleepers to be scheduled by the OS, run, find nothing left and decrement — so
        // every one of the beam's thousands of layers paid the wake-up latency of the
        // SLOWEST thread, even when the caller had already finished the layer itself.
        // Counting items instead makes a worker that never wakes simply not matter: the
        // caller keeps claiming until the work is gone, and a late waker finds m_next
        // past the end and goes straight back to sleep.
        if (m_done.load(std::memory_order_acquire) < count) {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvDone.wait(lk, [this, count] {
                return m_done.load(std::memory_order_acquire) >= count;
            });
        }
        m_fn = nullptr;
    }

private:
    // Dynamic claiming in small blocks. The per-item cost in the beam is one simulated
    // frame (~5 us) but it varies a lot — a state that dies immediately costs far less
    // than one that survives — so static ranges would leave workers idle at the tail.
    // A block amortises the atomic while keeping the tail short.
    static constexpr int kBlock = 16;

    void run(int worker) {
        for (;;) {
            // Lock-free claim: a mutex here serialised every worker on every block and
            // was a measurable part of the first (slower-than-serial) result.
            const int begin = m_next.fetch_add(kBlock, std::memory_order_relaxed);
            const int total = m_count.load(std::memory_order_relaxed);
            if (begin >= total) return;          // nothing claimed -> nothing to report
            const int endIdx = std::min(total, begin + kBlock);
            for (int i = begin; i < endIdx; ++i) (*m_fn)(i, worker);
            const int done = m_done.fetch_add(endIdx - begin, std::memory_order_acq_rel)
                             + (endIdx - begin);
            if (done >= total) {                 // last item of the batch: release the caller
                std::lock_guard<std::mutex> lk(m_mtx);
                m_cvDone.notify_one();
                return;
            }
        }
    }

    void loop(int worker) {
        unsigned long long seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cvWork.wait(lk, [this, &seen] { return m_stop || m_generation != seen; });
                if (m_stop) return;
                seen = m_generation;
            }
            run(worker);
        }
    }

    int m_workers = 1;
    std::vector<std::thread> m_threads;
    std::mutex m_mtx;
    std::condition_variable m_cvWork, m_cvDone;
    const std::function<void(int, int)>* m_fn = nullptr;
    // Atomic because a worker that wakes late reads it on the lock-free claim path
    // while the caller may already be setting up the next batch under the mutex.
    std::atomic<int> m_count{0};
    std::atomic<int> m_done{0};
    std::atomic<int> m_next{0};
    unsigned long long m_generation = 0;
    bool m_stop = false;
};

} // namespace gdsim
