#include "common.h"
#include <benchmark/benchmark.h>
#include <asio.hpp>
#include <thread>
#include <atomic>
#include <memory>

namespace {
    class TcpSession : public std::enable_shared_from_this<TcpSession> {
    public:
        explicit TcpSession(asio::ip::tcp::socket socket)
            : socket_(std::move(socket)) {
            // HFT optimization: disable Nagle's algorithm
            socket_.set_option(asio::ip::tcp::no_delay(true));
        }

        void start() {
            doRead();
        }

    private:
        void doRead() {
            auto self = shared_from_this();
            socket_.async_read_some(
                asio::buffer(buffer_),
                [this, self](std::error_code ec, std::size_t length) {
                    if (!ec) {
                        doWrite(length);
                    }
                });
        }

        void doWrite(std::size_t length) {
            auto self = shared_from_this();
            asio::async_write(
                socket_,
                asio::buffer(buffer_, length),
                [this, self](std::error_code ec, std::size_t) {
                    if (!ec) {
                        doRead();
                    }
                });
        }

        asio::ip::tcp::socket socket_;
        std::array<char, 8192> buffer_;
    };

    class TcpServer {
    public:
        TcpServer(asio::io_context& ioContext, std::uint16_t port)
            : acceptor_(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {
            doAccept();
        }

        void stop() {
            acceptor_.close();
        }

    private:
        void doAccept() {
            acceptor_.async_accept(
                [this](std::error_code ec, asio::ip::tcp::socket socket) {
                    if (!ec) {
                        std::make_shared<TcpSession>(std::move(socket))->start();
                    }
                    if (acceptor_.is_open()) {
                        doAccept();
                    }
                });
        }

        asio::ip::tcp::acceptor acceptor_;
    };

    // Global server state
    struct AsioServerState {
        std::unique_ptr<asio::io_context> ioContext;
        std::unique_ptr<TcpServer> server;
        std::thread serverThread;
        std::atomic<bool> running{false};

        void start() {
            if (running.exchange(true))
                return;

            ioContext = std::make_unique<asio::io_context>();
            server = std::make_unique<TcpServer>(*ioContext, bench::TCP_PORT + 1);

            serverThread = std::thread([this] {
                ioContext->run();
            });

            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }

        void stop() {
            if (!running.exchange(false))
                return;

            if (ioContext) {
                ioContext->stop();
            }
            if (serverThread.joinable()) {
                serverThread.join();
            }
            server.reset();
            ioContext.reset();
        }
    };

    AsioServerState g_asioServer;

    void runAsioEchoClient(std::size_t messageSize, std::size_t iterations) {
        asio::io_context ioContext;
        asio::ip::tcp::socket socket(ioContext);
        asio::ip::tcp::resolver resolver(ioContext);

        auto endpoints = resolver.resolve(bench::LOCALHOST, std::to_string(bench::TCP_PORT + 1));
        asio::connect(socket, endpoints);

        // HFT optimization: disable Nagle's algorithm
        socket.set_option(asio::ip::tcp::no_delay(true));

        auto payload = bench::makeStringPayload(messageSize);
        std::string response(messageSize, '\0');

        for (std::size_t i = 0; i < iterations; ++i) {
            asio::write(socket, asio::buffer(payload));
            asio::read(socket, asio::buffer(response));
        }

        socket.close();
    }
}

static void BM_AsioTcpEcho_64B(benchmark::State& state) {
    g_asioServer.start();
    for (auto _ : state) {
        runAsioEchoClient(64, state.range(0));
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    g_asioServer.stop();
}

static void BM_AsioTcpEcho_1KB(benchmark::State& state) {
    g_asioServer.start();
    for (auto _ : state) {
        runAsioEchoClient(1024, state.range(0));
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    g_asioServer.stop();
}

static void BM_AsioTcpEcho_64KB(benchmark::State& state) {
    g_asioServer.start();
    for (auto _ : state) {
        runAsioEchoClient(64 * 1024, state.range(0));
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    g_asioServer.stop();
}

BENCHMARK(BM_AsioTcpEcho_64B)->Arg(100)->Arg(1000);
BENCHMARK(BM_AsioTcpEcho_1KB)->Arg(100)->Arg(1000);
BENCHMARK(BM_AsioTcpEcho_64KB)->Arg(100)->Arg(1000);

class AsioTcpFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        g_asioServer.start();
    }

    void TearDown(const benchmark::State&) override {
        g_asioServer.stop();
    }
};

BENCHMARK_DEFINE_F(AsioTcpFixture, ThroughputTest)(benchmark::State& state) {
    const std::size_t messageSize = state.range(0);
    const std::size_t iterations = 100;

    for (auto _ : state) {
        runAsioEchoClient(messageSize, iterations);
    }

    state.SetBytesProcessed(state.iterations() * iterations * messageSize * 2);
}

BENCHMARK_REGISTER_F(AsioTcpFixture, ThroughputTest)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(65536);
