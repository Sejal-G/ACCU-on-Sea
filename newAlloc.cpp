#include <chrono>
#include <iostream>

int main() {
    const int N = 1'000'000;
    
    auto start = std::chrono::high_resolution_clock::now();
    for( int i = 0; i < N; i++ ) 
    {
        volatile int* p = new int( i );
        delete p;
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    std::cout << "1M allocations: "
              << std::chrono::duration_cast<std::chrono::milliseconds>( end - start ).count()
              << "ms\n";
}