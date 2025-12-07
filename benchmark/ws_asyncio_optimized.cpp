#include "common.h"
#include <benchmark/benchmark.h>
#include <asyncio/http/websocket.h>
#include <asyncio/event_loop.h>
#include <thread>
#include <atomic>

namespace {
    // Run multiple message exchanges on a single connection within one event loop
    asyncio::task::Task<void, std::error_code> runWsClientMultipleConnections(
        const std::string& url,
        std::size_t messageSize,
        std::size_t messagesPerConnection,
        std::size_t numConnections
    ) {
        auto payload = bench::makeStringPayload(messageSize);

        for (std::size_t c = 0; c < numConnections; ++c) {
            auto ws = co_await asyncio::http::ws::WebSocket::connect(
                *asyncio::http::URL::from(url)
            );
            CO_EXPECT(ws);

            for (std::size_t i = 0; i < messagesPerConnection; ++i) {
                CO_EXPECT(co_await ws->sendText(payload));
                auto msg = co_await ws->readMessage();
                CO_EXPECT(msg);
            }

            co_await ws->close(asyncio::http::ws::CloseCode::NORMAL_CLOSURE);
        }

        co_return {};
    }

    // Optimized: run entire benchmark iteration set in one event loop
    asyncio::task::Task<void, std::error_code> runWsBenchmarkIteration(
        const std::string& url,
        std::size_t messageSize,
        std::size_t iterations
    ) {
        auto ws = co_await asyncio::http::ws::WebSocket::connect(
            *asyncio::http::URL::from(url)
        );
        CO_EXPECT(ws);

        auto payload = bench::makeStringPayload(messageSize);

        for (std::size_t i = 0; i < iterations; ++i) {
            CO_EXPECT(co_await ws->sendText(payload));
            auto msg = co_await ws->readMessage();
            CO_EXPECT(msg);
        }

        co_await ws->close(asyncio::http::ws::CloseCode::NORMAL_CLOSURE);
        co_return {};
    }
}

// Optimized benchmark - single event loop for all iterations
static void BM_AsyncioWsOptimized_64B(benchmark::State& state) {
    const std::string url = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT) + "/";
    const std::size_t iterations = state.range(0);

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsBenchmarkIteration(url, 64, iterations);
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed - is echo server running?");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * iterations);
}

static void BM_AsyncioWsOptimized_1KB(benchmark::State& state) {
    const std::string url = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT) + "/";
    const std::size_t iterations = state.range(0);

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsBenchmarkIteration(url, 1024, iterations);
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * iterations);
}

static void BM_AsyncioWsOptimized_64KB(benchmark::State& state) {
    const std::string url = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT) + "/";
    const std::size_t iterations = state.range(0);

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsBenchmarkIteration(url, 64 * 1024, iterations);
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * iterations);
}

BENCHMARK(BM_AsyncioWsOptimized_64B)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK(BM_AsyncioWsOptimized_1KB)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK(BM_AsyncioWsOptimized_64KB)->Arg(10)->Arg(100);

class AsyncioWsOptimizedFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        url_ = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT) + "/";
    }

    std::string url_;
};

BENCHMARK_DEFINE_F(AsyncioWsOptimizedFixture, ThroughputTest)(benchmark::State& state) {
    const std::size_t messageSize = state.range(0);
    const std::size_t iterations = 100;

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> asyncio::task::Task<void, std::error_code> {
            auto ws = co_await asyncio::http::ws::WebSocket::connect(
                *asyncio::http::URL::from(url_)
            );
            CO_EXPECT(ws);

            auto payload = bench::makePayload(messageSize);

            for (std::size_t i = 0; i < iterations; ++i) {
                CO_EXPECT(co_await ws->sendBinary(payload));
                auto msg = co_await ws->readMessage();
                CO_EXPECT(msg);
            }

            co_await ws->close(asyncio::http::ws::CloseCode::NORMAL_CLOSURE);
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }

    state.SetBytesProcessed(state.iterations() * iterations * messageSize * 2);
}

BENCHMARK_REGISTER_F(AsyncioWsOptimizedFixture, ThroughputTest)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(65536);
