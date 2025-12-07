#include "common.h"
#include <benchmark/benchmark.h>
#include <asyncio/net/stream.h>
#include <asyncio/event_loop.h>
#include <thread>
#include <atomic>

namespace {
    std::atomic<bool> g_serverRunning{false};
    std::thread g_serverThread;

    asyncio::task::Task<void, std::error_code> handleClient(asyncio::net::TCPStream stream) {
        // HFT optimization: disable Nagle's algorithm for lower latency
        stream.noDelay(true);

        std::array<std::byte, 8192> buffer;

        while (true) {
            auto n = co_await stream.read(buffer);
            if (!n || *n == 0)
                break;

            auto writeResult = co_await stream.writeAll({buffer.data(), *n});
            if (!writeResult)
                break;
        }

        co_return {};
    }

    asyncio::task::Task<void, std::error_code> runServer() {
        auto listener = asyncio::net::TCPListener::listen(bench::LOCALHOST, bench::TCP_PORT);
        if (!listener)
            co_return std::unexpected{listener.error()};

        g_serverRunning = true;

        while (g_serverRunning) {
            auto stream = co_await listener->accept();
            if (!stream)
                break;

            handleClient(*std::move(stream)).future().fail([](const auto&) {});
        }

        co_await listener->close();
        co_return {};
    }

    void startAsyncioServer() {
        g_serverRunning = true;
        g_serverThread = std::thread([] {
            asyncio::run(runServer);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    void stopAsyncioServer() {
        g_serverRunning = false;
        // Connect to unblock accept
        try {
            asyncio::run([]() -> asyncio::task::Task<void, std::error_code> {
                auto stream = co_await asyncio::net::TCPStream::connect(bench::LOCALHOST, bench::TCP_PORT);
                if (stream)
                    co_await stream->close();
                co_return {};
            });
        } catch (...) {}

        if (g_serverThread.joinable())
            g_serverThread.join();
    }

    asyncio::task::Task<void, std::error_code> runEchoClient(std::size_t messageSize, std::size_t iterations) {
        auto stream = co_await asyncio::net::TCPStream::connect(bench::LOCALHOST, bench::TCP_PORT);
        CO_EXPECT(stream);

        // HFT optimization: disable Nagle's algorithm for lower latency
        stream->noDelay(true);

        auto payload = bench::makePayload(messageSize);
        std::vector<std::byte> response(messageSize);

        for (std::size_t i = 0; i < iterations; ++i) {
            CO_EXPECT(co_await stream->writeAll(payload));
            CO_EXPECT(co_await stream->readExactly(response));
        }

        co_await stream->close();
        co_return {};
    }
}

static void BM_AsyncioTcpEcho_64B(benchmark::State& state) {
    for (auto _ : state) {
        asyncio::run([&]() {
            return runEchoClient(64, state.range(0));
        });
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_AsyncioTcpEcho_1KB(benchmark::State& state) {
    for (auto _ : state) {
        asyncio::run([&]() {
            return runEchoClient(1024, state.range(0));
        });
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_AsyncioTcpEcho_64KB(benchmark::State& state) {
    for (auto _ : state) {
        asyncio::run([&]() {
            return runEchoClient(64 * 1024, state.range(0));
        });
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_AsyncioTcpEcho_64B)->Arg(100)->Arg(1000);
BENCHMARK(BM_AsyncioTcpEcho_1KB)->Arg(100)->Arg(1000);
BENCHMARK(BM_AsyncioTcpEcho_64KB)->Arg(100)->Arg(1000);

// Fixture for server lifecycle
class AsyncioTcpFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        startAsyncioServer();
    }

    void TearDown(const benchmark::State&) override {
        stopAsyncioServer();
    }
};

BENCHMARK_DEFINE_F(AsyncioTcpFixture, ThroughputTest)(benchmark::State& state) {
    const std::size_t messageSize = state.range(0);
    const std::size_t iterations = 100;

    for (auto _ : state) {
        asyncio::run([&]() {
            return runEchoClient(messageSize, iterations);
        });
    }

    state.SetBytesProcessed(state.iterations() * iterations * messageSize * 2);
}

BENCHMARK_REGISTER_F(AsyncioTcpFixture, ThroughputTest)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(65536);
