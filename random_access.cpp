#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

int main()
{
    constexpr size_t N = 50'000'000;

   std::vector<size_t> indices(N);

    std::iota(
        indices.begin(),
        indices.end(),
        0);

    std::shuffle(
        indices.begin(),
        indices.end(),
        std::mt19937{42});

    volatile long long sum = 0;

    auto start =
        std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < N; ++i)
        sum += indices[indices[i]];

    auto end =
        std::chrono::high_resolution_clock::now();

    std::cout
        << "Random: "
        << std::chrono::duration_cast<
               std::chrono::milliseconds>(
               end - start)
               .count()
        << " ms\n";
}