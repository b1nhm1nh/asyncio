#include "common.h"
#include <benchmark/benchmark.h>
#include <trantor/net/TcpServer.h>
#include <trantor/net/TcpClient.h>
#include <trantor/net/EventLoopThread.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace {
    // Global server state using trantor's event loop
    std::unique_ptr<trantor::EventLoopThread> g_serverLoop;
    std::unique_ptr<trantor::TcpServer> g_server;
    std::atomic<bool> g_serverRunning{false};

    void startServer() {
        if (g_serverRunning.exchange(true))
            return;

        g_serverLoop = std::make_unique<trantor::EventLoopThread>("TrantorServer");
        g_serverLoop->run();

        auto* loop = g_serverLoop->getLoop();
        std::promise<void> ready;

        loop->runInLoop([loop, &ready]() {
            trantor::InetAddress addr("127.0.0.1", bench::TCP_PORT + 2);
            g_server = std::make_unique<trantor::TcpServer>(loop, addr, "EchoServer");

            g_server->setConnectionCallback(
                [](const trantor::TcpConnectionPtr& conn) {
                    if (conn->connected()) {
                        // HFT optimization: disable Nagle's algorithm
                        conn->setTcpNoDelay(true);
                    }
                });

            g_server->setRecvMessageCallback(
                [](const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* buf) {
                    conn->send(buf->peek(), buf->readableBytes());
                    buf->retrieveAll();
                });

            g_server->start();
            ready.set_value();
        });

        ready.get_future().wait();
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    void stopServer() {
        if (!g_serverRunning.exchange(false))
            return;

        if (g_serverLoop && g_serverLoop->getLoop()) {
            std::promise<void> done;
            g_serverLoop->getLoop()->runInLoop([&done]() {
                if (g_server) {
                    g_server->stop();
                    g_server.reset();
                }
                done.set_value();
            });
            done.get_future().wait();
        }

        g_serverLoop.reset();
    }

    bool runTrantorEchoClient(std::size_t messageSize, std::size_t iterations) {
        std::string payload = bench::makeStringPayload(messageSize);
        std::atomic<std::size_t> completed{0};
        std::atomic<bool> done{false};
        std::atomic<bool> error{false};
        std::mutex mtx;
        std::condition_variable cv;

        trantor::EventLoopThread clientLoop("TrantorClient");
        clientLoop.run();

        auto* loop = clientLoop.getLoop();

        std::shared_ptr<trantor::TcpClient> client;

        loop->runInLoop([&]() {
            trantor::InetAddress serverAddr("127.0.0.1", bench::TCP_PORT + 2);
            client = std::make_shared<trantor::TcpClient>(loop, serverAddr, "EchoClient");

            client->setConnectionCallback(
                [&](const trantor::TcpConnectionPtr& conn) {
                    if (conn->connected()) {
                        // HFT optimization: disable Nagle's algorithm
                        conn->setTcpNoDelay(true);
                        conn->send(payload);
                    } else {
                        std::lock_guard lock(mtx);
                        done = true;
                        cv.notify_one();
                    }
                });

            client->setMessageCallback(
                [&](const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* buf) {
                    while (buf->readableBytes() >= messageSize) {
                        buf->retrieve(messageSize);
                        std::size_t count = ++completed;

                        if (count < iterations) {
                            conn->send(payload);
                        } else {
                            conn->shutdown();
                            std::lock_guard lock(mtx);
                            done = true;
                            cv.notify_one();
                            return;
                        }
                    }
                });

            client->setConnectionErrorCallback([&]() {
                std::lock_guard lock(mtx);
                error = true;
                done = true;
                cv.notify_one();
            });

            client->connect();
        });

        {
            std::unique_lock lock(mtx);
            cv.wait_for(lock, std::chrono::seconds{30}, [&] { return done.load(); });
        }

        loop->runInLoop([&client]() {
            if (client) {
                client->disconnect();
                client.reset();
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds{10});

        return !error && completed >= iterations;
    }
}

static void BM_TrantorTcpEcho_64B(benchmark::State& state) {
    startServer();
    for (auto _ : state) {
        if (!runTrantorEchoClient(64, state.range(0))) {
            state.SkipWithError("TCP connection failed");
            stopServer();
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    stopServer();
}

static void BM_TrantorTcpEcho_1KB(benchmark::State& state) {
    startServer();
    for (auto _ : state) {
        if (!runTrantorEchoClient(1024, state.range(0))) {
            state.SkipWithError("TCP connection failed");
            stopServer();
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    stopServer();
}

static void BM_TrantorTcpEcho_64KB(benchmark::State& state) {
    startServer();
    for (auto _ : state) {
        if (!runTrantorEchoClient(64 * 1024, state.range(0))) {
            state.SkipWithError("TCP connection failed");
            stopServer();
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    stopServer();
}

BENCHMARK(BM_TrantorTcpEcho_64B)->Arg(100)->Arg(1000);
BENCHMARK(BM_TrantorTcpEcho_1KB)->Arg(100)->Arg(1000);
BENCHMARK(BM_TrantorTcpEcho_64KB)->Arg(100)->Arg(1000);

class TrantorTcpFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        startServer();
    }

    void TearDown(const benchmark::State&) override {
        stopServer();
    }
};

BENCHMARK_DEFINE_F(TrantorTcpFixture, ThroughputTest)(benchmark::State& state) {
    const std::size_t messageSize = state.range(0);
    const std::size_t iterations = 100;

    for (auto _ : state) {
        if (!runTrantorEchoClient(messageSize, iterations)) {
            state.SkipWithError("TCP connection failed");
            return;
        }
    }

    state.SetBytesProcessed(state.iterations() * iterations * messageSize * 2);
}

BENCHMARK_REGISTER_F(TrantorTcpFixture, ThroughputTest)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(65536);
