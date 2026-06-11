/**
 * heap_aging.cpp  —  CppCon edition
 *
 * Measures allocator degradation over simulated time under two pressures:
 *
 *   HOT SET      — fixed pool of small objects, churned (free+alloc) every
 *                  epoch.  We time a full scan each epoch.  Flat vector of
 *                  pointers: no linked-list pointer-chasing confound.
 *
 *   COLD WINDOW  — sliding window of larger objects.  Once full, each new
 *                  alloc is paired with a free of the oldest object.  Live
 *                  memory is BOUNDED after warm-up, so any RSS growth is
 *                  pure allocator overhead (fragmentation), not working-set
 *                  growth.
 *
 * Build:
 *   g++ -O2 -std=c++17 -o heap_aging heap_aging.cpp
 *
 * Run:
 *   ./heap_aging <label> [duration_seconds]
 *
 *   duration_seconds defaults to 43200 (12 simulated hours).
 *   Ctrl-C stops it at any time; TSV is flushed every epoch.
 *
 * Examples:
 *   ./heap_aging system  3600 > results_system.tsv
 *   LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 \
 *       ./heap_aging jemalloc 3600 > results_jemalloc.tsv
 *   LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc.so.4 \
 *       ./heap_aging tcmalloc 3600 > results_tcmalloc.tsv
 *
 * Or just: bash run_all.sh 3600
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
/*  Platform: clock + RSS                                               */
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
/*  Tunables                                                            */
/* ------------------------------------------------------------------ */

// duration_seconds is now argv[2] at runtime — no recompile needed.
static constexpr uint64_t EPOCH_SECONDS   = 300;    // 5 sim-minutes per report

static constexpr uint32_t HOT_COUNT       = 4096;   // hot objects alive at once
static constexpr uint32_t HOT_BYTES       = 64;     // bytes per hot object
static constexpr double   HOT_CHURN_RATE  = 0.10;   // fraction replaced per epoch

static constexpr uint32_t COLD_WINDOW     = 2000;   // max live cold objects
static constexpr uint32_t COLD_PER_EPOCH  = 17;     // cold allocs per epoch
static constexpr uint32_t COLD_BYTES      = 256;    // bytes per cold object

static constexpr uint32_t SCAN_REPS       = 17;     // 1 cold-cache discard + 16 timed
static constexpr uint32_t RNG_SEED        = 0xDEADBEEF;

/* ------------------------------------------------------------------ */
/*  Object layouts                                                      */
/* ------------------------------------------------------------------ */

struct HotObj  { uint32_t id; uint8_t data[HOT_BYTES];  };
struct ColdObj { uint32_t id; uint8_t data[COLD_BYTES]; };

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

static void hot_churn(std::vector<HotObj*>& hot, uint32_t n, std::mt19937& rng) {
    std::uniform_int_distribution<uint32_t> pick(0, HOT_COUNT - 1);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t idx    = pick(rng);
        uint32_t old_id = hot[idx]->id;
        delete hot[idx];
        hot[idx] = new HotObj();
        hot[idx]->id = old_id;
        memset(hot[idx]->data, (uint8_t)old_id, HOT_BYTES);
    }
}

// First rep is cold-cache warmup (discarded). Returns avg ns over warm reps.
static uint64_t hot_scan(const std::vector<HotObj*>& hot) {
    volatile uint64_t sink = 0;
    for (uint32_t i = 0; i < HOT_COUNT; ++i) sink += hot[i]->id;  // cold discard

    uint64_t t0 = now_ns();
    for (uint32_t r = 1; r < SCAN_REPS; ++r)
        for (uint32_t i = 0; i < HOT_COUNT; ++i)
            sink += hot[i]->id;
    (void)sink;
    return (now_ns() - t0) / (SCAN_REPS - 1);
}

/* ------------------------------------------------------------------ */
/*  Cold sliding window                                                 */
/* ------------------------------------------------------------------ */

static void cold_step(std::vector<ColdObj*>& win, uint32_t& head, uint64_t& total) {
    if (win.size() < COLD_WINDOW) {
        ColdObj* o = new ColdObj();
        o->id = (uint32_t)(total++);
        memset(o->data, (uint8_t)o->id, COLD_BYTES);
        win.push_back(o);
    } else {
        // Evict oldest, allocate newest into same slot — allocator must reuse.
        delete win[head];
        ColdObj* o = new ColdObj();
        o->id = (uint32_t)(total++);
        memset(o->data, (uint8_t)o->id, COLD_BYTES);
        win[head] = o;
        head = (head + 1) % COLD_WINDOW;
    }
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char* argv[]) {
    const std::string label         = (argc > 1) ? argv[1] : "unknown";
    const uint64_t    total_seconds = (argc > 2)
                                        ? (uint64_t)strtoull(argv[2], nullptr, 10)
                                        : 43200ULL;   // default: 12 sim-hours

    fprintf(stderr,
        "[heap_aging] allocator=%-10s  duration=%" PRIu64 "s (%.2f sim-days)\n"
        "             hot=%u×%uB churn=%.0f%%  "
        "cold_window=%u×%uB cold_per_epoch=%u\n"
        "             Ctrl-C at any time — TSV is flushed every epoch.\n",
        label.c_str(), total_seconds, (double)total_seconds / 86400.0,
        HOT_COUNT, HOT_BYTES, HOT_CHURN_RATE * 100.0,
        COLD_WINDOW, COLD_BYTES, COLD_PER_EPOCH);

    std::mt19937 rng(RNG_SEED);

    std::vector<HotObj*>  hot;
    hot_init(hot);

    std::vector<ColdObj*> cold_win;
    cold_win.reserve(COLD_WINDOW);
    uint32_t cold_head  = 0;
    uint64_t cold_total = 0;

    printf("tick_s\tday\tallocator\trss_kb\t"
           "scan_ns_total\tscan_ns_per_obj\t"
           "hot_live\tcold_live\tcold_total_allocd\n");
    fflush(stdout);

    const uint32_t n_churn = (uint32_t)(HOT_COUNT * HOT_CHURN_RATE);
    uint64_t tick = 0;

    while (tick < total_seconds) {
        for (uint64_t t = 0; t < EPOCH_SECONDS && tick < total_seconds; ++t, ++tick)
            hot_churn(hot, n_churn, rng);

        for (uint32_t c = 0; c < COLD_PER_EPOCH; ++c)
            cold_step(cold_win, cold_head, cold_total);

        uint64_t scan_ns = hot_scan(hot);
        uint64_t rss     = read_rss_kb();
        double   day     = (double)tick / 86400.0;

        printf("%" PRIu64 "\t%.4f\t%s\t%" PRIu64 "\t"
               "%" PRIu64 "\t%.3f\t"
               "%" PRIu32 "\t%zu\t%" PRIu64 "\n",
               tick, day, label.c_str(), rss,
               scan_ns, (double)scan_ns / HOT_COUNT,
               HOT_COUNT, cold_win.size(), cold_total);
        fflush(stdout);
    }

    for (auto* p : hot)      delete p;
    for (auto* p : cold_win) delete p;

    fprintf(stderr, "[heap_aging] done. cold_total=%" PRIu64 "  cold_live=%zu\n",
            cold_total, cold_win.size());
    return 0;
}