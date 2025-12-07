#include "common.h"
#include <benchmark/benchmark.h>
#include <drogon/WebSocketClient.h>
#include <drogon/HttpAppFramework.h>
#include <trantor/net/EventLoopThread.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace {
    // Use trantor's EventLoopThread instead of drogon::app().run()
    std::unique_ptr<trantor::EventLoopThread> g_loopThread;

    void ensureLoopRunning() {
        if (!g_loopThread) {
            g_loopThread = std::make_unique<trantor::EventLoopThread>("DrogonBench");
            g_loopThread->run();
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }

    trantor::EventLoop* getLoop() {
        ensureLoopRunning();
        return g_loopThread->getLoop();
    }

    class WsClientBenchmark {
    public:
        WsClientBenchmark(const std::string& url, std::size_t messageSize, std::size_t iterations)
            : url_(url)
            , payload_(bench::makeStringPayload(messageSize))
            , iterations_(iterations)
            , completed_(0)
            , done_(false)
            , error_(false) {}

        bool run() {
            auto loop = getLoop();

            loop->runInLoop([this]() {
                wsClient_ = drogon::WebSocketClient::newWebSocketClient(url_, getLoop());

                wsClient_->setMessageHandler([this](const std::string&,
                                                    const drogon::WebSocketClientPtr& client,
                                                    const drogon::WebSocketMessageType& type) {
                    if (type == drogon::WebSocketMessageType::Text ||
                        type == drogon::WebSocketMessageType::Binary) {
                        ++completed_;
                        if (completed_ < iterations_) {
                            client->getConnection()->send(payload_);
                        } else {
                            client->getConnection()->shutdown();
                            std::lock_guard lock(mutex_);
                            done_ = true;
                            cv_.notify_one();
                        }
                    }
                });

                wsClient_->setConnectionClosedHandler([this](const drogon::WebSocketClientPtr&) {
                    std::lock_guard lock(mutex_);
                    done_ = true;
                    cv_.notify_one();
                });

                auto req = drogon::HttpRequest::newHttpRequest();
                req->setPath("/");

                wsClient_->connectToServer(
                    req,
                    [this](drogon::ReqResult result,
                           const drogon::HttpResponsePtr&,
                           const drogon::WebSocketClientPtr& client) {
                        if (result == drogon::ReqResult::Ok) {
                            client->getConnection()->send(payload_);
                        } else {
                            std::lock_guard lock(mutex_);
                            error_ = true;
                            done_ = true;
                            cv_.notify_one();
                        }
                    });
            });

            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds{30}, [this] { return done_.load(); });

            return !error_ && completed_ >= iterations_;
        }

    private:
        std::string url_;
        std::string payload_;
        std::size_t iterations_;
        std::atomic<std::size_t> completed_;
        std::atomic<bool> done_;
        std::atomic<bool> error_;
        std::mutex mutex_;
        std::condition_variable cv_;
        drogon::WebSocketClientPtr wsClient_;
    };
}

static void BM_DrogonWsText_64B(benchmark::State& state) {
    ensureLoopRunning();
    const std::string url = "ws://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT);

    for (auto _ : state) {
        WsClientBenchmark client(url, 64, state.range(0));
        if (!client.run()) {
            state.SkipWithError("WebSocket connection failed - is echo server running?");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_DrogonWsText_1KB(benchmark::State& state) {
    ensureLoopRunning();
    const std::string url = "ws://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT);

    for (auto _ : state) {
        WsClientBenchmark client(url, 1024, state.range(0));
        if (!client.run()) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_DrogonWsBinary_64B(benchmark::State& state) {
    ensureLoopRunning();
    const std::string url = "ws://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT);

    for (auto _ : state) {
        WsClientBenchmark client(url, 64, state.range(0));
        if (!client.run()) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_DrogonWsBinary_64KB(benchmark::State& state) {
    ensureLoopRunning();
    const std::string url = "ws://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT);

    for (auto _ : state) {
        WsClientBenchmark client(url, 64 * 1024, state.range(0));
        if (!client.run()) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_DrogonWsText_64B)->Arg(10)->Arg(100);
BENCHMARK(BM_DrogonWsText_1KB)->Arg(10)->Arg(100);
BENCHMARK(BM_DrogonWsBinary_64B)->Arg(10)->Arg(100);
BENCHMARK(BM_DrogonWsBinary_64KB)->Arg(10)->Arg(100);

class DrogonWsFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        url_ = "ws://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT);
        ensureLoopRunning();
    }

    std::string url_;
};

BENCHMARK_DEFINE_F(DrogonWsFixture, ThroughputTest)(benchmark::State& state) {
    const std::size_t messageSize = state.range(0);
    const std::size_t iterations = 50;

    for (auto _ : state) {
        WsClientBenchmark client(url_, messageSize, iterations);
        if (!client.run()) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }

    state.SetBytesProcessed(state.iterations() * iterations * messageSize * 2);
}

BENCHMARK_REGISTER_F(DrogonWsFixture, ThroughputTest)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(65536);
