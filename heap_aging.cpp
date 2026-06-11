/**
 * heap_aging.cpp
 *
 * Heap Aging Experiment — CppCon edition
 *
 * Measures how much a heap allocator degrades over time under two
 * concurrent pressures that real long-running programs produce:
 *
 *   HOT SET  — a fixed-size pool of small objects that are replaced
 *              (free + alloc) at a steady churn rate every epoch.
 *              We time a full scan of all live hot objects each epoch.
 *              No linked list: objects live in a flat std::vector of
 *              pointers, so pointer chasing is NOT a confound.
 *
 *   COLD WINDOW — a sliding window of larger objects that accumulates
 *              slowly and evicts the oldest object each time a new one
 *              is added (once the window is full).  The allocator must
 *              reuse freed slots.  Fragmentation shows up as degraded
 *              reuse quality (higher RSS, slower hot-set scans) over
 *              simulated time even though live memory is BOUNDED.
 *
 * Why bounded cold objects matter:
 *   If cold objects are never freed the RSS grows forever, which
 *   conflates simple memory growth with fragmentation.  A sliding
 *   window keeps peak live memory constant after warm-up, so any
 *   RSS growth or latency increase is purely allocator overhead.
 *
 * TSV output columns:
 *   tick_s  day  allocator  rss_kb
 *   scan_ns_total  scan_ns_per_obj
 *   hot_live  cold_live  cold_total_allocd
 *
 * Build on Linux / WSL:
 *   g++ -O2 -std=c++17 -o heap_aging heap_aging.cpp
 *
 * Run (pass allocator name as argv[1] for labelling in TSV):
 *   ./heap_aging system   > results_system.tsv
 *   LD_PRELOAD=/path/to/libjemalloc.so ./heap_aging jemalloc > results_jemalloc.tsv
 *   LD_PRELOAD=/path/to/libtcmalloc.so ./heap_aging tcmalloc > results_tcmalloc.tsv
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

/* ------------------------------------------------------------------ */
/*  Platform: clock + RSS (Linux / WSL only)                           */
/* ------------------------------------------------------------------ */

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t read_rss_kb() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    uint64_t rss = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, " %" SCNu64, &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

/* ------------------------------------------------------------------ */
/*  Tunables — change these to stress-test different scenarios         */
/* ------------------------------------------------------------------ */

// Simulated clock
static constexpr uint64_t TOTAL_SECONDS  = 5ULL * 24 * 3600;  // 5 simulated days
static constexpr uint64_t EPOCH_SECONDS  = 300;                 // report every 5 sim-minutes

// Hot set: many small short-lived objects, scanned every epoch
static constexpr uint32_t HOT_COUNT      = 4096;   // objects alive at any time
static constexpr uint32_t HOT_BYTES      = 64;     // bytes per hot object
static constexpr double   HOT_CHURN_RATE = 0.10;   // fraction replaced per epoch

// Cold window: larger objects, one added + one evicted per step (once full)
static constexpr uint32_t COLD_WINDOW    = 2000;   // max live cold objects (bounded!)
static constexpr uint32_t COLD_PER_EPOCH = 17;     // new cold objects allocated per epoch
static constexpr uint32_t COLD_BYTES     = 256;    // bytes per cold object

// Scan timing: first rep is cold-cache, rest are averaged
static constexpr uint32_t SCAN_REPS      = 17;     // 1 cold + 16 warm

static constexpr uint32_t RNG_SEED       = 0xDEADBEEF;

/* ------------------------------------------------------------------ */
/*  Object layouts                                                      */
/* ------------------------------------------------------------------ */

struct HotObj {
    uint32_t id;
    uint8_t  data[HOT_BYTES];
};

struct ColdObj {
    uint32_t id;
    uint8_t  data[COLD_BYTES];
};

/* ------------------------------------------------------------------ */
/*  Hot set helpers                                                     */
/* ------------------------------------------------------------------ */

// Allocate HOT_COUNT fresh objects, store pointers in hot[].
static void hot_init(std::vector<HotObj*>& hot) {
    hot.resize(HOT_COUNT);
    for (uint32_t i = 0; i < HOT_COUNT; ++i) {
        hot[i] = new HotObj();
        hot[i]->id = i;
        memset(hot[i]->data, (uint8_t)i, HOT_BYTES);
    }
}

// Replace n_replace randomly chosen objects (free old, alloc new).
// The vector itself is never resized — only the pointed-to objects change.
// This is the fragmentation driver: freed slots may or may not be reused
// well depending on the allocator.
static void hot_churn(std::vector<HotObj*>& hot, uint32_t n_replace,
                      std::mt19937& rng) {
    std::uniform_int_distribution<uint32_t> pick(0, HOT_COUNT - 1);
    for (uint32_t i = 0; i < n_replace; ++i) {
        uint32_t idx = pick(rng);
        uint32_t old_id = hot[idx]->id;
        delete hot[idx];

        hot[idx] = new HotObj();
        hot[idx]->id = old_id;   // preserve id so scan result is stable
        memset(hot[idx]->data, (uint8_t)old_id, HOT_BYTES);
    }
}

// Scan all HOT_COUNT objects.  First rep is discarded (cold cache).
// Returns the average ns for one full scan over the warm reps.
static uint64_t hot_scan(const std::vector<HotObj*>& hot) {
    volatile uint64_t sink = 0;

    // Cold-cache rep — discard timing
    for (uint32_t i = 0; i < HOT_COUNT; ++i) sink += hot[i]->id;

    uint64_t t0 = now_ns();
    for (uint32_t r = 1; r < SCAN_REPS; ++r)
        for (uint32_t i = 0; i < HOT_COUNT; ++i)
            sink += hot[i]->id;

    (void)sink;
    return (now_ns() - t0) / (SCAN_REPS - 1);
}

/* ------------------------------------------------------------------ */
/*  Cold window helpers                                                 */
/* ------------------------------------------------------------------ */

// Add one cold object.  If the window is full, evict the oldest first.
// Using a simple ring-buffer index into a fixed-capacity vector.
static void cold_step(std::vector<ColdObj*>& cold_window,
                      uint32_t& cold_head,   // index of oldest object
                      uint64_t& cold_total,  // total ever allocated
                      std::mt19937& /*rng*/) {
    if (cold_window.size() < COLD_WINDOW) {
        // Still filling the window — just append
        ColdObj* obj = new ColdObj();
        obj->id = (uint32_t)(cold_total++);
        memset(obj->data, (uint8_t)obj->id, COLD_BYTES);
        cold_window.push_back(obj);
    } else {
        // Window is full: free the oldest, reuse its slot
        // This is the key fix — the allocator must reclaim and reuse memory.
        delete cold_window[cold_head];
        ColdObj* obj = new ColdObj();
        obj->id = (uint32_t)(cold_total++);
        memset(obj->data, (uint8_t)obj->id, COLD_BYTES);
        cold_window[cold_head] = obj;
        cold_head = (cold_head + 1) % COLD_WINDOW;  // advance ring
    }
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char* argv[]) {
    const std::string label = (argc > 1) ? argv[1] : "unknown";

    fprintf(stderr,
        "[heap_aging] allocator=%-10s  hot=%u×%uB churn=%.0f%%  "
        "cold_window=%u×%uB cold_per_epoch=%u\n",
        label.c_str(),
        HOT_COUNT, HOT_BYTES, HOT_CHURN_RATE * 100.0,
        COLD_WINDOW, COLD_BYTES, COLD_PER_EPOCH);

    std::mt19937 rng(RNG_SEED);

    // --- Hot set setup ---
    std::vector<HotObj*> hot;
    hot_init(hot);

    // --- Cold window setup ---
    std::vector<ColdObj*> cold_window;
    cold_window.reserve(COLD_WINDOW);
    uint32_t cold_head  = 0;
    uint64_t cold_total = 0;

    // --- TSV header ---
    printf("tick_s\tday\tallocator\trss_kb\t"
           "scan_ns_total\tscan_ns_per_obj\t"
           "hot_live\tcold_live\tcold_total_allocd\n");
    fflush(stdout);

    const uint32_t n_churn = (uint32_t)(HOT_COUNT * HOT_CHURN_RATE);
    uint64_t tick = 0;

    while (tick < TOTAL_SECONDS) {
        // ---- Epoch: advance simulated time ----
        for (uint64_t t = 0; t < EPOCH_SECONDS && tick < TOTAL_SECONDS; ++t, ++tick) {
            hot_churn(hot, n_churn, rng);
        }

        // ---- Allocate cold objects this epoch ----
        for (uint32_t c = 0; c < COLD_PER_EPOCH; ++c)
            cold_step(cold_window, cold_head, cold_total, rng);

        // ---- Measure ----
        uint64_t scan_ns = hot_scan(hot);
        uint64_t rss     = read_rss_kb();
        double   day     = (double)tick / (24.0 * 3600.0);

        printf("%" PRIu64 "\t%.4f\t%s\t%" PRIu64 "\t"
               "%" PRIu64 "\t%.3f\t"
               "%" PRIu32 "\t%zu\t%" PRIu64 "\n",
               tick, day, label.c_str(), rss,
               scan_ns, (double)scan_ns / HOT_COUNT,
               HOT_COUNT,
               cold_window.size(),
               cold_total);
        fflush(stdout);
    }

    // Cleanup
    for (auto* p : hot)         delete p;
    for (auto* p : cold_window) delete p;

    fprintf(stderr, "[heap_aging] done. cold_total_allocd=%" PRIu64 "  cold_live=%zu\n",
            cold_total, cold_window.size());
    return 0;
}
