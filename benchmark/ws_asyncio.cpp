#include "common.h"
#include <benchmark/benchmark.h>
#include <asyncio/http/websocket.h>
#include <asyncio/event_loop.h>

namespace {
    // Note: These benchmarks require an external WebSocket echo server running
    // on WS_PORT. You can use any echo server like:
    // - websocat -s 18081
    // - or the sample ws server

    asyncio::task::Task<void, std::error_code> runWsClient(
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

    asyncio::task::Task<void, std::error_code> runWsBinaryClient(
        const std::string& url,
        std::size_t messageSize,
        std::size_t iterations
    ) {
        auto ws = co_await asyncio::http::ws::WebSocket::connect(
            *asyncio::http::URL::from(url)
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
    }
}

static void BM_AsyncioWsText_64B(benchmark::State& state) {
    // asyncio uses http:// scheme for WebSocket (not ws://)
    const std::string url = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT) + "/";

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsClient(url, 64, state.range(0));
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed - is echo server running?");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_AsyncioWsText_1KB(benchmark::State& state) {
    const std::string url = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT) + "/";

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsClient(url, 1024, state.range(0));
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_AsyncioWsBinary_64B(benchmark::State& state) {
    const std::string url = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT) + "/";

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsBinaryClient(url, 64, state.range(0));
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_AsyncioWsBinary_64KB(benchmark::State& state) {
    const std::string url = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT) + "/";

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsBinaryClient(url, 64 * 1024, state.range(0));
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_AsyncioWsText_64B)->Arg(10)->Arg(100);
BENCHMARK(BM_AsyncioWsText_1KB)->Arg(10)->Arg(100);
BENCHMARK(BM_AsyncioWsBinary_64B)->Arg(10)->Arg(100);
BENCHMARK(BM_AsyncioWsBinary_64KB)->Arg(10)->Arg(100);

class AsyncioWsFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        url_ = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT) + "/";
    }

    std::string url_;
};

BENCHMARK_DEFINE_F(AsyncioWsFixture, ThroughputTest)(benchmark::State& state) {
    const std::size_t messageSize = state.range(0);
    const std::size_t iterations = 50;

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsBinaryClient(url_, messageSize, iterations);
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }

    state.SetBytesProcessed(state.iterations() * iterations * messageSize * 2);
}

BENCHMARK_REGISTER_F(AsyncioWsFixture, ThroughputTest)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(65536);
