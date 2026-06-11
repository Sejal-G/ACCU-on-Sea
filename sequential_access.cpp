#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

int main()
{
    constexpr size_t N = 50'000'000;

    std::vector<int> data(N);

    std::iota(data.begin(), data.end(), 0);

    volatile long long sum = 0;

    auto start =
        std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < N; ++i)
        sum += data[i];

    auto end =
        std::chrono::high_resolution_clock::now();

    std::cout
        << "Sequential: "
        << std::chrono::duration_cast<
               std::chrono::milliseconds>(
               end - start)
               .count()
        << " ms\n";
}