# Allocator Experiments

These experiments accompany my talk on allocator architecture, locality, and long-running system behavior.

The goal is not to benchmark a particular allocator implementation. The goal is to demonstrate how allocator behavior changes as workloads evolve.

---

# Experiment 1 — The Myth of `new`

## Objective

Demonstrate that allocation can appear extremely cheap when the workload is allocator-friendly.

This experiment repeatedly allocates and immediately deallocates the same object type.

```cpp
for ( int i = 0; i < 1'000'000; ++i ) {
    auto* p = new int( i );
    delete p;
}
```

## Hypothesis

The allocator should be able to satisfy most requests from its fastest path due to:

* Fixed allocation size
* Immediate reuse
* No contention
* Predictable allocation pattern

## Environment

* Compiler: MSVC (Release, /O2)
* Machine: Acer Aspire A315-58, Intel Core i5-1135G7, 4 cores, 8 GB RAM, Windows 10 Home 64-bit

## Results

| Metric              | Value     |
| ------------------- | --------- |
| Allocations         | 1,000,000 |
| Average Runtime     | 45 ms     |

## Observation

One million allocations complete in 45 ms.

This corresponds to roughly 20 million allocations per second.

The allocator is not constantly requesting memory from the operating system. It is primarily reusing memory that it already owns.

## Takeaway

Allocation cost is not an intrinsic property of `new`.

The observed cost depends heavily on whether the workload stays on the allocator's fast path.

---

# Experiment 2 — Same Allocator, Different Workload

## Objective

Demonstrate how allocator behavior changes when allocation patterns become less predictable.

Characteristics:

* 8 worker threads
* Random allocation sizes (8–512 bytes)
* Random lifetimes
* Concurrent activity

## Hypothesis

Even though the allocation count remains unchanged, allocator performance should degrade due to:

* Thread contention
* Reduced reuse opportunities
* Increased pressure on shared allocator structures

## Environment

* Compiler: MSVC (Release, /O2)
* Machine: Acer Aspire A315-58, Intel Core i5-1135G7, 4 cores / 8 threads, 8 GB RAM, Windows 10 Home 64-bit

## Results

| Metric      | Value     |
| ----------- | --------- |
| Threads     | 8         |
| Allocations | 1,000,000 |
| Runtime     | 165 ms    |

## Comparison

| Scenario     | Runtime |
| ------------ | ------- |
| Experiment 1 | 45 ms   |
| Experiment 2 | 165 ms  |

Approximate slowdown:

3.7×

## Observation

The allocator implementation did not change.

The machine did not change.

The allocation count did not change.

Only the workload changed.

## Takeaway

Allocators optimize patterns, not allocations.

Fast paths are highly dependent on predictable allocation behavior.

When those patterns disappear, performance can degrade substantially even though the allocator itself is unchanged.

---

# Experiment 3 — The Three-Tier Cliff

## Objective

The purpose is to visualize the increasing cost of falling through allocator layers.

---

## Tier 1 — Thread-Local Cache

Workload:

* Single thread
* Fixed-size allocation
* Immediate reuse

Result:

| Metric  | Value |
| ------- | ----- |
| Runtime | 45 ms |

Characteristics:

* No synchronization
* Excellent reuse
* Hot allocator state

---

## Tier 2 — Shared Arena

Workload:

* 8 threads
* Random allocation sizes
* Random lifetimes

Result:

| Metric  | Value  |
| ------- | ------ |
| Runtime | 165 ms |

Characteristics:

* Increased coordination
* Shared allocator structures
* Reduced locality

---

## Tier 3 — OS Boundary

Workload:

* Large allocations
* Explicit page touching
* Forced page population

Example:

```cpp
constexpr size_t GB = 1024ull * 1024 * 1024;

char* p = new char[ GB ];

for ( size_t i = 0; i < GB; i += 4096 )
    p[ i ] = 1;
```

Result:

| Metric  | Value     |
| ------- | --------- |
| Runtime | [Measure] |

Characteristics:

* Virtual memory interaction
* Page population
* Potential page faults
* Kernel involvement

---

## Conceptual Model

| Tier               | Relative Cost        |
| ------------------ | -------------------- |
| Thread-local cache | 1×                   |
| Shared arena       | ~4×                  |
| OS boundary        | Significantly higher |

Exact values vary by allocator, operating system, hardware, and workload.

The important observation is the existence of a performance cliff rather than the specific numerical value.

---

# Overall Conclusion

The allocator is not a single mechanism.

It is a hierarchy of increasingly expensive fallback paths.

Most microbenchmarks measure only the fastest tier.

Production systems inevitably exercise all tiers.

Understanding when a workload falls through those tiers is often more important than understanding allocator implementation details.


# Experiment 4 — Allocation Tail Latency

## Objective

Demonstrate that allocator performance is not adequately described by averages.

Most allocations are extremely fast. A small number are significantly slower.

This experiment measures the latency of every allocation and computes percentile statistics.

---

## Motivation

Many production systems monitor average latency.

Unfortunately, averages can hide allocator behavior that only appears in the tail.

The goal of this experiment is to show that:

* P50 can remain stable
* P99 can increase
* P99.9 can increase dramatically
* Worst-case allocations can be orders of magnitude slower

All at the same time.

---

## Methodology

For each allocation:

```cpp
auto start = Clock::now();

auto* p = new int(i);

auto end = Clock::now();

delete p;
```

Record:

```text
allocation latency
```

Repeat:

```text
500,000 times
```

Sort the collected measurements and compute:

* P50
* P99
* P99.9
* Maximum observed latency

---

## Environment

* Compiler: MSVC (Release, /O2)
* Machine: Acer Aspire A315-58, Intel Core i5-1135G7, 4 cores / 8 threads, 8 GB RAM, Windows 10 Home 64-bit

---

## Results

### Run 1

| Percentile | Latency    |
| ---------- | ---------- |
| P50        | 100 ns     |
| P99        | 300 ns     |
| P99.9      | 800 ns     |
| Max        | 111,800 ns |

### Run 2

| Percentile | Latency   |
| ---------- | --------- |
| P50        | 100 ns    |
| P99        | 200 ns    |
| P99.9      | 800 ns    |
| Max        | 55,400 ns |

---

## Visualization

| Percentile | Relative Cost |
| ---------- | ------------- |
| P50        | 1×            |
| P99        | 2–3×          |
| P99.9      | 8×            |
| Max        | 500–1100×     |

---

## Observation

Most allocations complete in approximately:

```text
100 ns
```

This suggests allocation is essentially free.

However, the slowest observed allocation was:

```text
111.8 us
```

or roughly:

```text
1118× slower than P50
```

Both statements are true simultaneously.

---

## Why This Happens

Most allocations are serviced by the allocator's fast path:

* Thread-local caches
* Warm free lists
* Reused memory

Occasionally allocations encounter:

* Arena contention
* Cache misses
* Page faults
* Internal allocator maintenance
* OS interaction

These rare events dominate tail latency.

---

## Takeaway

Average latency hides allocator behavior.

A service can appear healthy when observing P50 while simultaneously accumulating severe tail latency.

This is one of the reasons long-running performance issues are difficult to diagnose.

The machine is experiencing allocator pressure even when the average says everything is fine.

---

## Connection to the Talk

This experiment motivates the next section:

> The average looks healthy.
>
> The tail is a disaster.

The allocator is not a single fast operation.

It is a hierarchy of fast paths and increasingly expensive fallback paths.

Percentile measurements reveal that hierarchy.
