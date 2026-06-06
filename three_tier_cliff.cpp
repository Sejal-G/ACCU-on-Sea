#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

constexpr int OPS = 1'000'000;

void tier1()
{
    auto start = Clock::now();

    for (int i = 0; i < OPS; ++i)
    {
        auto* p = new int(i);
        delete p;
    }

    auto end = Clock::now();

    std::cout
        << "Tier 1 (Thread-local style): "
        << std::chrono::duration_cast<std::chrono::milliseconds>(
               end - start)
               .count()
        << " ms\n";
}

void worker()
{
    std::mt19937 rng(std::random_device{}());

    std::uniform_int_distribution<size_t> size_dist(8, 512);

    std::vector<void*> live;

    for (int i = 0; i < OPS / 8; ++i)
    {
        auto sz = size_dist(rng);

        live.push_back(::operator new(sz));

        if (live.size() > 1000)
        {
            size_t idx = rng() % live.size();

            ::operator delete(live[idx]);

            live[idx] = live.back();
            live.pop_back();
        }
    }

    for (auto p : live)
        ::operator delete(p);
}

void tier2()
{
    auto start = Clock::now();

    std::vector<std::thread> threads;

    for (int i = 0; i < 8; ++i)
        threads.emplace_back(worker);

    for (auto& t : threads)
        t.join();

    auto end = Clock::now();

    std::cout
        << "Tier 2 (Arena pressure): "
        << std::chrono::duration_cast<std::chrono::milliseconds>(
               end - start)
               .count()
        << " ms\n";
}

void tier3()
{
    constexpr size_t GB = 1024ull * 1024 * 1024;

    auto start = Clock::now();

    char* p = new char[GB];

    for (size_t i = 0; i < GB; i += 4096)
        p[i] = 1;

    auto end = Clock::now();

    std::cout
        << "Tier 3 (OS boundary): "
        << std::chrono::duration_cast<std::chrono::milliseconds>(
               end - start)
               .count()
        << " ms\n";

    delete[] p;
}

int main()
{
    tier1();
    tier2();
    tier3();
}