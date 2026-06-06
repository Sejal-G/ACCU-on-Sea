#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

int main()
{
    constexpr int N = 500000;

    std::vector<long long> latencies;

    latencies.reserve(N);

    for (int i = 0; i < N; ++i)
    {
        auto start = Clock::now();

        auto* p = new int(i);

        auto end = Clock::now();

        delete p;

        auto ns =
            std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                end - start)
                .count();

        latencies.push_back(ns);
    }

    std::sort(
        latencies.begin(),
        latencies.end());

    auto p50 =
        latencies[N * 50 / 100];

    auto p99 =
        latencies[N * 99 / 100];

    auto p999 =
        latencies[N * 999 / 1000];

    auto max =
        latencies.back();

    std::cout << "P50   : " << p50 << " ns\n";
    std::cout << "P99   : " << p99 << " ns\n";
    std::cout << "P99.9 : " << p999 << " ns\n";
    std::cout << "Max   : " << max << " ns\n";
}