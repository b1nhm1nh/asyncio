#include "common.h"
#include <benchmark/benchmark.h>
#include <drogon/WebSocketClient.h>
#include <drogon/WebSocketController.h>
#include <drogon/HttpAppFramework.h>
#include <trantor/net/EventLoopThread.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace {
    // WebSocket Echo Controller for Drogon server
    class EchoWebSocket : public drogon::WebSocketController<EchoWebSocket> {
    public:
        void handleNewMessage(
            const drogon::WebSocketConnectionPtr& wsConnPtr,
            std::string&& message,
            const drogon::WebSocketMessageType& type
        ) override {
            // Echo the message back
            if (type == drogon::WebSocketMessageType::Text) {
                wsConnPtr->send(message);
            } else if (type == drogon::WebSocketMessageType::Binary) {
                wsConnPtr->send(message);
            }
        }

        void handleNewConnection(
            const drogon::HttpRequestPtr&,
            const drogon::WebSocketConnectionPtr&
        ) override {}

        void handleConnectionClosed(const drogon::WebSocketConnectionPtr&) override {}

        WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/", drogon::Get);
        WS_PATH_LIST_END
    };

    // Use trantor's EventLoopThread for client event loop (same as ws_drogon.cpp)
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

    // Client for benchmarking (reuse from ws_drogon.cpp pattern)
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
            trantor::EventLoopThread clientLoop("DrogonClient");
            clientLoop.run();

            auto* loop = clientLoop.getLoop();

            loop->runInLoop([this, loop]() {
                wsClient_ = drogon::WebSocketClient::newWebSocketClient(url_, loop);

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

// Drogon client benchmarks using asyncio's internal WS server (port WS_PORT + 1)
// This allows fair comparison: both clients use the same asyncio server
static void BM_DrogonWsVsAsyncioServer_64B(benchmark::State& state) {
    ensureLoopRunning();
    // Uses the asyncio WS server started by ws_server_asyncio.cpp (port WS_PORT + 1)
    const std::string url = "ws://127.0.0.1:" + std::to_string(bench::WS_PORT + 1);

    for (auto _ : state) {
        WsClientBenchmark client(url, 64, state.range(0));
        if (!client.run()) {
            state.SkipWithError("WebSocket connection failed - ensure asyncio server tests run first");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_DrogonWsVsAsyncioServer_1KB(benchmark::State& state) {
    ensureLoopRunning();
    const std::string url = "ws://127.0.0.1:" + std::to_string(bench::WS_PORT + 1);

    for (auto _ : state) {
        WsClientBenchmark client(url, 1024, state.range(0));
        if (!client.run()) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_DrogonWsVsAsyncioServer_64KB(benchmark::State& state) {
    ensureLoopRunning();
    const std::string url = "ws://127.0.0.1:" + std::to_string(bench::WS_PORT + 1);

    for (auto _ : state) {
        WsClientBenchmark client(url, 64 * 1024, state.range(0));
        if (!client.run()) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}

// NOTE: These benchmarks require the asyncio WS server to be running
// Run BM_AsyncioWsInternal_* benchmarks first to start the server
BENCHMARK(BM_DrogonWsVsAsyncioServer_64B)->Arg(10)->Arg(100);
BENCHMARK(BM_DrogonWsVsAsyncioServer_1KB)->Arg(10)->Arg(100);
BENCHMARK(BM_DrogonWsVsAsyncioServer_64KB)->Arg(10)->Arg(100);
