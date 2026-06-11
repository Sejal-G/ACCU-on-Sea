#include <list>
#include <vector>
#include <chrono>
#include <iostream>
#include <fstream>
#include <random>
#include <thread>
#include <atomic>
#include <ctime>
#include <sys/resource.h>

// ─── RSS from /proc ───────────────────────────────────────
long get_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            sscanf(line.c_str(), "VmRSS: %ld", &kb);
            return kb;
        }
    return -1;
}

// ─── Heap aging thread ────────────────────────────────────
// Runs continuously in background.
// Allocates objects of mixed sizes and lifetimes,
// keeps ~30% alive at all times to pin holes in place.
std::atomic<bool> running{true};

void heap_ager() {
    std::mt19937 rng(42);
    std::vector<void*> survivors;
    survivors.reserve(50000);

    while (running) {
        // Allocate 500 objects, random sizes 8–512 bytes
        std::vector<void*> batch;
        batch.reserve(500);
        for (int i = 0; i < 500; i++) {
            size_t sz = 8 + (rng() % 504);
            batch.push_back(::operator new(sz));
        }

        // Free 70% immediately — punches holes
        for (int i = 0; i < 350; i++)
            ::operator delete(batch[i]);

        // Keep 30% — pins holes in place
        for (int i = 350; i < 500; i++)
            survivors.push_back(batch[i]);

        // Periodically free some survivors too —
        // simulates objects with mixed longer lifetimes
        if (survivors.size() > 20000) {
            for (int i = 0; i < 2000; i++) {
                ::operator delete(survivors[i]);
            }
            survivors.erase(survivors.begin(),
                           survivors.begin() + 2000);
        }

        // Small sleep — don't peg the CPU
        std::this_thread::sleep_for(
            std::chrono::microseconds(100));
    }
}

// ─── Measurement ─────────────────────────────────────────
struct Metrics {
    long   fresh_us;   // traversal on fresh allocation
    long   aged_us;    // traversal on heap-aged allocation
    float  slowdown;
    long   rss_kb;
    double hours_elapsed;
};

Metrics measure(int N, double hours) {
    // Fresh: allocate a new list right now, measure it
    {
        std::list<int> warmup;
        for (int i = 0; i < 1000; i++) warmup.push_back(i);
    } // freed immediately — just to touch the allocator

    // Fresh list — allocated into whatever state heap is in
    // We call this "fresh" meaning: allocated just now,
    // not traversed before
    auto build_and_measure = [&]() {
        std::list<int> lst;
        for (int i = 0; i < N; i++) lst.push_back(i);
        auto t1 = std::chrono::high_resolution_clock::now();
        volatile long sum = 0;
        for (int x : lst) sum += x;
        auto t2 = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast
                   std::chrono::microseconds>(t2-t1).count();
    };

    // Run 5 times, take median — removes outliers
    std::vector<long> samples;
    for (int i = 0; i < 5; i++)
        samples.push_back(build_and_measure());
    std::sort(samples.begin(), samples.end());
    long median = samples[2];

    Metrics m;
    m.aged_us       = median;
    m.fresh_us      = samples[0]; // best case this hour
    m.slowdown      = (float)median / samples[0];
    m.rss_kb        = get_rss_kb();
    m.hours_elapsed = hours;
    return m;
}

// ─── Timestamp helper ─────────────────────────────────────
std::string now_str() {
    auto t  = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// ─── Main ─────────────────────────────────────────────────
int main() {
    const int    N            = 500'000;
    const int    HOURS        = 120;      // 5 days
    const int    LOG_EVERY_S  = 3600;    // every hour
    const std::string logfile = "heap_aging.csv";

    // Write CSV header
    std::ofstream csv(logfile);
    csv << "timestamp,hours_elapsed,rss_kb,"
        << "median_us,best_us,slowdown_x\n";
    csv.flush();

    std::cout << "Starting 5-day heap aging experiment.\n"
              << "Logging to: " << logfile << "\n"
              << "Total hours: " << HOURS << "\n\n";

    // Baseline — before aging starts
    auto baseline = measure(N, 0.0);
    std::cout << "[Hour 0] RSS: " << baseline.rss_kb
              << " KB | Median: " << baseline.aged_us
              << " us (baseline)\n";
    csv << now_str() << ",0," << baseline.rss_kb << ","
        << baseline.aged_us << "," << baseline.fresh_us
        << ",1.0\n";
    csv.flush();

    // Start the background heap ager
    std::thread ager(heap_ager);

    auto start = std::chrono::steady_clock::now();

    for (int hour = 1; hour <= HOURS; hour++) {
        // Sleep until next measurement
        std::this_thread::sleep_until(
            start + std::chrono::seconds(
                (long long)hour * LOG_EVERY_S));

        double elapsed = hour; // exactly N hours
        auto m = measure(N, elapsed);

        float slowdown_vs_baseline =
            (float)m.aged_us / baseline.aged_us;

        // Log to CSV
        csv << now_str()       << ","
            << elapsed         << ","
            << m.rss_kb        << ","
            << m.aged_us       << ","
            << m.fresh_us      << ","
            << slowdown_vs_baseline << "\n";
        csv.flush(); // flush every hour — survives crashes

        // Print to terminal
        std::cout << "[Hour " << hour << "] "
                  << now_str()
                  << " | RSS: "   << m.rss_kb/1024 << " MB"
                  << " | Median: "<< m.aged_us      << " us"
                  << " | Slowdown: "
                  << slowdown_vs_baseline << "x\n";
    }

    running = false;
    ager.join();

    std::cout << "\nExperiment complete. Data in: "
              << logfile << "\n";
    return 0;
}