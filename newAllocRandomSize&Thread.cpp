#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

constexpr int THREADS = 8;
constexpr int OPS_PER_THREAD = 125000; // 8 * 125k = 1M allocations

std::atomic<size_t> total_allocations { 0 };

void worker( int seed )
{
    std::mt19937 rng( seed );

    std::uniform_int_distribution<size_t> size_dist( 8, 512 );
    std::uniform_int_distribution<int> action_dist( 0, 1 );

    std::vector<void*> live;

    live.reserve( 10000 );

    for ( int i = 0; i < OPS_PER_THREAD; ++i )
    {
        size_t sz = size_dist( rng );

        void* p = ::operator new( sz );

        live.push_back( p );

        total_allocations++;

        // randomly free an older allocation
        if ( !live.empty() && action_dist( rng ) )
        {
            size_t idx = rng() % live.size();

            ::operator delete( live[idx] );

            live[idx] = live.back();
            live.pop_back();
        }
    }

    for ( void* p : live )
    {
        ::operator delete( p );
    }
}

int main()
{
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;

    for ( int i = 0; i < THREADS; ++i )
    {
        threads.emplace_back( worker, i );
    }

    for ( auto& t : threads )
    {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();

    std::cout
        << "Allocations: "
        << total_allocations.load()
        << "\n";

    std::cout
        << "Time: "
        << std::chrono::duration_cast<std::chrono::milliseconds>( end - start ).count()
        << " ms\n";
}
