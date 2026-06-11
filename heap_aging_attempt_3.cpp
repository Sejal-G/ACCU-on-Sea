/**
 * heap_aging.cpp  —  CppCon edition (v3)
 *
 * What this measures
 * ==================
 * Real allocator degradation under sustained mixed-size churn.
 * Three pressures run simultaneously, as they do in real servers:
 *
 *   HOT SET     — 65536 small objects (64B each, ~4MB working set).
 *                 This exceeds typical L2 but fits in L3. After
 *                 fragmentation develops, objects scatter across pages
 *                 and scan latency rises — this is the signal.
 *
 *   COLD WINDOW — sliding window of mixed-size objects (32B / 256B / 2KB
 *                 in equal thirds, picked randomly). Mixed sizes force
 *                 the allocator to juggle multiple size-class bins and
 *                 create irregular holes — the realistic fragmentation
 *                 pattern. Window size is fixed so live memory is bounded.
 *
 *   CHURN       — 10% of the hot set is replaced every epoch. Each free
 *                 creates a hole; the allocator must fill it on the next
 *                 alloc. Poor bin-matching = objects placed far apart =
 *                 cache misses = measurable latency increase.
 *
 * Why wall-clock time
 * ===================
 * tick++ in a tight loop is fake time — the program finishes in seconds
 * regardless of "simulated days". This version runs for a real duration
 * (argv[2] seconds, default 120s) so the allocator actually sustains
 * pressure. Use longer runs (600s, 3600s) for the talk.
 *
 * Build:
 *   g++ -O2 -std=c++17 -o heap_aging heap_aging.cpp
 *
 * Run:
 *   ./heap_aging system  120 > results_system.tsv
 *   LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 \
 *       ./heap_aging jemalloc 120 > results_jemalloc.tsv
 *   LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc.so.4 \
 *       ./heap_aging tcmalloc 120 > results_tcmalloc.tsv
 *
 *   Or just:  bash run_all.sh 120
 *
 * TSV columns:
 *   elapsed_s  allocator  rss_kb
 *   scan_ns_total  scan_ns_per_obj
 *   total_allocs  cold_live
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <ctime>
#include <vector>
#include <string>
#include <random>
#include <atomic>
#include <signal.h>

/* ------------------------------------------------------------------ */
/*  Stop flag — set by SIGALRM or Ctrl-C                               */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

/* ------------------------------------------------------------------ */
/*  Clock + RSS                                                         */
/* ------------------------------------------------------------------ */

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
}

static double elapsed_s(uint64_t start_ns) {
    return (double)(now_ns() - start_ns) / 1e9;
}

static uint64_t read_rss_kb() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256]; uint64_t rss = 0;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "VmRSS:", 6) == 0) { sscanf(line+6, " %" SCNu64, &rss); break; }
    fclose(f);
    return rss;
}

/* ------------------------------------------------------------------ */
/*  Tunables                                                            */
/* ------------------------------------------------------------------ */

// Hot set: large enough to exceed L2, stress allocator placement
static constexpr uint32_t HOT_COUNT      = 65536;   // ~4MB working set at 64B each
static constexpr uint32_t HOT_BYTES      = 64;
static constexpr double   HOT_CHURN_RATE = 0.10;    // 10% replaced per epoch

// Cold window: mixed sizes to stress multiple allocator size classes
// Sizes chosen to span small/medium/large bin boundaries in all three allocators
static constexpr uint32_t COLD_WINDOW    = 8000;    // live cold objects (bounded)
static constexpr uint32_t COLD_PER_EPOCH = 200;     // allocs per epoch

static const uint32_t COLD_SIZES[]       = {32, 256, 2048};  // bytes, picked uniformly
static constexpr uint32_t N_COLD_SIZES   = 3;

// Epoch: how often we measure. Short enough to see early-time behaviour.
static constexpr uint32_t EPOCH_ALLOCS   = 5000;   // allocs between measurements
static constexpr uint32_t SCAN_REPS      = 9;      // 1 cold-discard + 8 timed
static constexpr uint32_t RNG_SEED       = 0xDEADBEEF;

/* ------------------------------------------------------------------ */
/*  Object layouts                                                      */
/* ------------------------------------------------------------------ */

struct HotObj { uint32_t id; uint8_t data[HOT_BYTES]; };

// Cold objects are variable size — we heap-allocate raw bytes and
// store (ptr, size) so we can free correctly.
struct ColdSlot { void* ptr; uint32_t size; };

/* ------------------------------------------------------------------ */
/*  Hot set                                                             */
/* ------------------------------------------------------------------ */

static void hot_init(std::vector<HotObj*>& hot) {
    hot.resize(HOT_COUNT);
    for (uint32_t i = 0; i < HOT_COUNT; ++i) {
        hot[i] = new HotObj();
        hot[i]->id = i;
        memset(hot[i]->data, (uint8_t)i, HOT_BYTES);
    }
}

static void hot_churn(std::vector<HotObj*>& hot, uint32_t n,
                      std::mt19937& rng, uint64_t& total_allocs) {
    std::uniform_int_distribution<uint32_t> pick(0, HOT_COUNT - 1);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t idx = pick(rng);
        uint32_t old_id = hot[idx]->id;
        delete hot[idx];
        hot[idx] = new HotObj();
        hot[idx]->id = old_id;
        memset(hot[idx]->data, (uint8_t)old_id, HOT_BYTES);
        ++total_allocs;
    }
}

// Scan: pointer-dereference through all hot objects.
// Allocator fragmentation scatters objects across pages → cache misses → higher latency.
static uint64_t hot_scan(const std::vector<HotObj*>& hot) {
    volatile uint64_t sink = 0;
    // Cold-cache warmup rep — discard timing
    for (uint32_t i = 0; i < HOT_COUNT; ++i) sink += hot[i]->id;

    uint64_t t0 = now_ns();
    for (uint32_t r = 1; r < SCAN_REPS; ++r)
        for (uint32_t i = 0; i < HOT_COUNT; ++i)
            sink += hot[i]->id;
    (void)sink;
    return (now_ns() - t0) / (SCAN_REPS - 1);
}

/* ------------------------------------------------------------------ */
/*  Cold sliding window — mixed sizes                                   */
/* ------------------------------------------------------------------ */

static void cold_step(std::vector<ColdSlot>& win, uint32_t& head,
                      uint64_t& total_allocs, std::mt19937& rng) {
    std::uniform_int_distribution<uint32_t> size_pick(0, N_COLD_SIZES - 1);
    uint32_t sz = COLD_SIZES[size_pick(rng)];

    if (win.size() < COLD_WINDOW) {
        void* p = malloc(sz);
        memset(p, 0xAB, sz);
        win.push_back({p, sz});
    } else {
        free(win[head].ptr);
        void* p = malloc(sz);
        memset(p, 0xAB, sz);
        win[head] = {p, sz};
        head = (head + 1) % COLD_WINDOW;
    }
    ++total_allocs;
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char* argv[]) {
    const std::string label        = (argc > 1) ? argv[1] : "unknown";
    const uint64_t    duration_s   = (argc > 2)
                                     ? (uint64_t)strtoull(argv[2], nullptr, 10)
                                     : 120ULL;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr,
        "[heap_aging] allocator=%-10s  duration=%" PRIu64 "s\n"
        "             hot=%u×%uB (%.1fMB working set) churn=%.0f%%\n"
        "             cold_window=%u mixed-size objects (%u/%u/%uB)\n"
        "             Ctrl-C stops cleanly.\n",
        label.c_str(), duration_s,
        HOT_COUNT, HOT_BYTES, (double)HOT_COUNT * HOT_BYTES / (1024*1024),
        HOT_CHURN_RATE * 100.0,
        COLD_WINDOW, COLD_SIZES[0], COLD_SIZES[1], COLD_SIZES[2]);

    std::mt19937 rng(RNG_SEED);

    std::vector<HotObj*> hot;
    hot_init(hot);

    std::vector<ColdSlot> cold_win;
    cold_win.reserve(COLD_WINDOW);
    uint32_t cold_head   = 0;
    uint64_t total_allocs = (uint64_t)HOT_COUNT;

    printf("elapsed_s\tallocator\trss_kb\t"
           "scan_ns_total\tscan_ns_per_obj\t"
           "total_allocs\tcold_live\n");
    fflush(stdout);

    const uint32_t n_churn = (uint32_t)(HOT_COUNT * HOT_CHURN_RATE);
    const uint64_t start   = now_ns();
    uint64_t epoch_allocs  = 0;

    while (!g_stop && elapsed_s(start) < (double)duration_s) {
        // Churn hot set
        hot_churn(hot, n_churn, rng, total_allocs);
        epoch_allocs += n_churn;

        // Churn cold window
        for (uint32_t c = 0; c < COLD_PER_EPOCH; ++c)
            cold_step(cold_win, cold_head, total_allocs, rng);
        epoch_allocs += COLD_PER_EPOCH;

        // Report every EPOCH_ALLOCS allocations
        if (epoch_allocs >= EPOCH_ALLOCS) {
            epoch_allocs = 0;
            uint64_t scan_ns = hot_scan(hot);
            uint64_t rss     = read_rss_kb();
            double   secs    = elapsed_s(start);

            printf("%.3f\t%s\t%" PRIu64 "\t"
                   "%" PRIu64 "\t%.3f\t"
                   "%" PRIu64 "\t%zu\n",
                   secs, label.c_str(), rss,
                   scan_ns, (double)scan_ns / HOT_COUNT,
                   total_allocs, cold_win.size());
            fflush(stdout);
        }
    }

    // Cleanup
    for (auto* p : hot)       delete p;
    for (auto& s : cold_win)  free(s.ptr);

    fprintf(stderr, "[heap_aging] done. elapsed=%.1fs  total_allocs=%" PRIu64 "\n",
            elapsed_s(start), total_allocs);
    return 0;
}
