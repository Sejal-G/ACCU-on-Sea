/**
 * heap_aging_experiment.cpp
 *
 * "I left a process running for seven days.
 *  I asked you to imagine a high-throughput service serving
 *  loads of requests every second. I wrote a skeleton for the same."
 *
 * What this does:
 *   - Simulates a high-throughput C++ service skeleton
 *     (parse → process → serialize → cleanup, per request)
 *   - A background thread ages the heap continuously:
 *     mixed sizes, mixed lifetimes, random free order
 *   - Every hour: measures list traversal time + RSS
 *   - Logs to CSV so you can chart two graphs:
 *       1. Traversal slowdown over time
 *       2. RSS over time
 *
 * Build:
 *   g++ -O2 -std=c++17 -pthread heap_aging_experiment.cpp \
 *       -o heap_experiment
 *
 * Run (background, survives terminal close):
 *   nohup ./heap_experiment > experiment_log.txt 2>&1 &
 *   echo $! > experiment.pid
 *
 * Check progress:
 *   tail -f experiment_log.txt
 *
 * Stop early:
 *   kill $(cat experiment.pid)
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <list>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────
// RSS — reads physical memory from /proc/self/status
// This is the graph your audience will recognise immediately.
// "The allocator is consuming more physical memory than the
//  working set requires, because fragmentation prevents it
//  from packing objects densely."
// ─────────────────────────────────────────────────────────
long get_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            sscanf(line.c_str(), "VmRSS: %ld", &kb);
            return kb;
        }
    }
    return -1;
}

// ─────────────────────────────────────────────────────────
// The "service skeleton"
//
// "Every request allocates — temp strings, parsed payloads,
//  serialization buffers, ownership wrappers."
//
// This isn't a real HTTP server. It's a faithful skeleton
// of what every real service does per request.
// ─────────────────────────────────────────────────────────
struct ParsedPayload {
    std::string  header;       // temp string
    std::string  body;         // parsed payload
    std::vector<int> metadata; // metadata object
};

struct SerializedResponse {
    std::string buffer;        // serialization buffer
};

ParsedPayload  simulate_parse(int req_id) {
    ParsedPayload p;
    p.header   = "request-" + std::to_string(req_id);
    p.body     = std::string(64 + (req_id % 128), 'x');
    p.metadata.resize(8 + (req_id % 16), req_id);
    return p;  // allocates, then caller owns
}

int simulate_process(const ParsedPayload& p) {
    volatile int result = 0;
    for (int x : p.metadata) result += x;
    return result;
}

SerializedResponse simulate_serialize(int result, int req_id) {
    SerializedResponse r;
    r.buffer = "resp:" + std::to_string(result)
             + ":id:"  + std::to_string(req_id);
    return r;
}

void simulate_send(const SerializedResponse&) {
    // no-op — we care about the allocation pattern, not I/O
}

// One complete request cycle — allocate, use, free
void handle_request(int req_id) {
    auto req    = simulate_parse(req_id);
    auto result = simulate_process(req);
    auto resp   = simulate_serialize(result, req_id);
    simulate_send(resp);
    // req, result, resp all freed here — different lifetimes
}

// ─────────────────────────────────────────────────────────
// Background heap ager
//
// "A background thread continuously allocates objects of
//  random sizes between 8 and 512 bytes, simulating the
//  mixed allocation patterns of a real service.
//  Frees ~70% immediately — creates holes.
//  Keeps ~30% alive — pins holes in place."
//
// This is what makes the heap age. It runs for the entire
// 120 hours without stopping.
// ─────────────────────────────────────────────────────────
std::atomic<bool> running{true};

void heap_ager() {
    std::mt19937 rng(42);
    std::vector<void*> survivors;
    survivors.reserve(60000);

    while (running) {

        // ── Service traffic simulation ──
        // Handle a burst of requests — this is the "hundreds
        // of thousands of requests per second" skeleton
        for (int i = 0; i < 200; i++) {
            handle_request(rng() % 100000);
        }

        // ── Raw heap churn ──
        // Mixed sizes, like real allocator pressure from
        // strings, vectors, shared_ptr control blocks, etc.
        std::vector<void*> batch;
        batch.reserve(500);
        for (int i = 0; i < 500; i++) {
            size_t sz = 8 + (rng() % 504); // 8–512 bytes
            batch.push_back(::operator new(sz));
        }

        // Free 70% immediately — punches holes in the heap
        for (int i = 0; i < 350; i++)
            ::operator delete(batch[i]);

        // Keep 30% alive — pins those holes in place
        // so they cannot be reused contiguously
        for (int i = 350; i < 500; i++)
            survivors.push_back(batch[i]);

        // Periodically free some survivors —
        // simulates objects with mixed longer lifetimes.
        // "Different lifetimes become interleaved."
        if (survivors.size() > 30000) {
            for (size_t i = 0; i < 3000; i++)
                ::operator delete(survivors[i]);
            survivors.erase(
                survivors.begin(),
                survivors.begin() + 3000);
        }

        // Breathe — don't peg the CPU
        std::this_thread::sleep_for(
            std::chrono::microseconds(200));
    }
}

// ─────────────────────────────────────────────────────────
// Measurement
//
// "The same list traversal that took 18 microseconds at
//  hour 0 may take 140+ microseconds at hour 120.
//  Same code. Same list size."
//
// We build a std::list AFTER the heap has aged, so nodes
// land in the fragmented holes the ager left behind.
// Traversal time is a direct proxy for layout quality.
// ─────────────────────────────────────────────────────────
struct Sample {
    long best_us;    // fastest run — the fast path
    long median_us;  // middle run  — the typical case
    long worst_us;   // slowest run — the tail
};

Sample measure_traversal(int N) {
    std::vector<long> times;
    times.reserve(7);

    for (int run = 0; run < 7; run++) {
        // Build list now — lands in whatever holes exist
        std::list<int> lst;
        for (int i = 0; i < N; i++) lst.push_back(i);

        auto t1 = std::chrono::high_resolution_clock::now();
        volatile long sum = 0;
        for (int x : lst) sum += x;
        auto t2 = std::chrono::high_resolution_clock::now();

        times.push_back(
            std::chrono::duration_cast<
                std::chrono::microseconds>(t2 - t1).count());
    }

    std::sort(times.begin(), times.end());
    return { times[0], times[3], times[6] };
}

// ─────────────────────────────────────────────────────────
// Timestamp
// ─────────────────────────────────────────────────────────
std::string now_str() {
    auto t  = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// ─────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {

    // Optional: pass allocator name as arg for labelling
    // e.g.  LD_PRELOAD=libjemalloc.so ./heap_experiment jemalloc
    std::string allocator_name = "default";
    if (argc > 1) allocator_name = argv[1];

    const int    N            = 500'000;  // list size
    const int    HOURS        = 120;      // 5 days
    const int    LOG_EVERY_S  = 3600;    // once per hour
    const std::string logfile =
        "heap_aging_" + allocator_name + ".csv";

    std::ofstream csv(logfile);
    csv << "allocator,timestamp,hours_elapsed,"
        << "rss_kb,best_us,median_us,worst_us,"
        << "slowdown_vs_baseline\n";
    csv.flush();

    std::cout << "===========================================\n"
              << " Heap Aging Experiment\n"
              << " Allocator : " << allocator_name << "\n"
              << " List size : " << N << " elements\n"
              << " Duration  : " << HOURS << " hours\n"
              << " Log file  : " << logfile << "\n"
              << "===========================================\n\n";

    // ── Hour 0: baseline before ager starts ──
    auto baseline = measure_traversal(N);
    long baseline_median = baseline.median_us;

    std::cout << "[Hour 0 — BASELINE]\n"
              << "  RSS     : " << get_rss_kb() << " KB\n"
              << "  Best    : " << baseline.best_us   << " us\n"
              << "  Median  : " << baseline.median_us << " us\n"
              << "  Worst   : " << baseline.worst_us  << " us\n"
              << "  Slowdown: 1.00x (this is the reference)\n\n";

    csv << allocator_name << ","
        << now_str()      << ",0,"
        << get_rss_kb()   << ","
        << baseline.best_us   << ","
        << baseline.median_us << ","
        << baseline.worst_us  << ","
        << "1.00\n";
    csv.flush();

    // ── Start the background heap ager ──
    // "The workload itself isn't particularly interesting.
    //  What interested me was the trend."
    std::thread ager(heap_ager);

    auto experiment_start = std::chrono::steady_clock::now();

    // ── Hourly measurement loop ──
    for (int hour = 1; hour <= HOURS; hour++) {

        // Sleep until this hour's measurement window
        std::this_thread::sleep_until(
            experiment_start +
            std::chrono::seconds((long long)hour * LOG_EVERY_S));

        auto s = measure_traversal(N);
        long rss = get_rss_kb();
        float slowdown = (float)s.median_us / baseline_median;

        // Log to CSV — flushed every hour,
        // so a crash loses at most one data point
        csv << allocator_name << ","
            << now_str()      << ","
            << hour           << ","
            << rss            << ","
            << s.best_us      << ","
            << s.median_us    << ","
            << s.worst_us     << ","
            << slowdown       << "\n";
        csv.flush();

        // Print to terminal
        std::cout << "[Hour " << hour << "] "
                  << now_str() << "\n"
                  << "  RSS     : " << rss/1024 << " MB\n"
                  << "  Best    : " << s.best_us   << " us\n"
                  << "  Median  : " << s.median_us << " us\n"
                  << "  Worst   : " << s.worst_us  << " us\n"
                  << "  Slowdown: " << slowdown << "x"
                  << " vs hour-0 baseline\n\n";
    }

    running = false;
    ager.join();

    std::cout << "Experiment complete.\n"
              << "Data written to: " << logfile << "\n\n"
              << "Two graphs to make from the CSV:\n"
              << "  1. slowdown_vs_baseline  over hours_elapsed\n"
              << "  2. rss_kb                over hours_elapsed\n";
    return 0;
}