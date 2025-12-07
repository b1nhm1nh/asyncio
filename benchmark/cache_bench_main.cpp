#include <benchmark/benchmark.h>

// Cleanup functions from benchmark files
extern void cleanupRedis();
extern void cleanupMemcached();

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    // Cleanup connections
    cleanupRedis();
    cleanupMemcached();

    return 0;
}
