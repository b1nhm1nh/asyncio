#include "common.h"
#include <benchmark/benchmark.h>
#include <asio.hpp>
#include <thread>
#include <atomic>
#include <memory>

namespace {
    // Async server (same as before)
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
        std::array<char, 65536> buffer_;
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

    // Async client
    class AsyncEchoClient : public std::enable_shared_from_this<AsyncEchoClient> {
    public:
        AsyncEchoClient(asio::io_context& io, const std::string& host, uint16_t port,
                        std::size_t messageSize, std::size_t iterations)
            : socket_(io)
            , resolver_(io)
            , payload_(bench::makeStringPayload(messageSize))
            , response_(messageSize, '\0')
            , iterations_(iterations)
            , completed_(0)
            , messageSize_(messageSize) {}

        void start(std::function<void(bool)> callback) {
            callback_ = std::move(callback);
            auto self = shared_from_this();
            resolver_.async_resolve(
                bench::LOCALHOST, std::to_string(bench::TCP_PORT + 3),
                [this, self](std::error_code ec, asio::ip::tcp::resolver::results_type results) {
                    if (ec) {
                        callback_(false);
                        return;
                    }
                    asio::async_connect(
                        socket_, results,
                        [this, self](std::error_code ec, const asio::ip::tcp::endpoint&) {
                            if (ec) {
                                callback_(false);
                                return;
                            }
                            // HFT optimization: disable Nagle's algorithm
                            socket_.set_option(asio::ip::tcp::no_delay(true));
                            doWrite();
                        });
                });
        }

    private:
        void doWrite() {
            auto self = shared_from_this();
            asio::async_write(
                socket_,
                asio::buffer(payload_),
                [this, self](std::error_code ec, std::size_t) {
                    if (ec) {
                        callback_(false);
                        return;
                    }
                    doRead();
                });
        }

        void doRead() {
            auto self = shared_from_this();
            asio::async_read(
                socket_,
                asio::buffer(response_),
                [this, self](std::error_code ec, std::size_t) {
                    if (ec) {
                        callback_(false);
                        return;
                    }
                    ++completed_;
                    if (completed_ < iterations_) {
                        doWrite();
                    } else {
                        socket_.close();
                        callback_(true);
                    }
                });
        }

        asio::ip::tcp::socket socket_;
        asio::ip::tcp::resolver resolver_;
        std::string payload_;
        std::string response_;
        std::size_t iterations_;
        std::size_t completed_;
        std::size_t messageSize_;
        std::function<void(bool)> callback_;
    };

    // Global async server state
    struct AsioAsyncServerState {
        std::unique_ptr<asio::io_context> ioContext;
        std::unique_ptr<TcpServer> server;
        std::thread serverThread;
        std::atomic<bool> running{false};

        void start() {
            if (running.exchange(true))
                return;

            ioContext = std::make_unique<asio::io_context>();
            server = std::make_unique<TcpServer>(*ioContext, bench::TCP_PORT + 3);

            serverThread = std::thread([this] {
                auto work = asio::make_work_guard(*ioContext);
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

    AsioAsyncServerState g_asioAsyncServer;

    bool runAsioAsyncEchoClient(std::size_t messageSize, std::size_t iterations) {
        asio::io_context clientIo;
        bool success = false;
        bool done = false;

        auto client = std::make_shared<AsyncEchoClient>(
            clientIo, bench::LOCALHOST, bench::TCP_PORT + 3, messageSize, iterations);

        client->start([&](bool result) {
            success = result;
            done = true;
        });

        clientIo.run();
        return success;
    }
}

static void BM_AsioAsyncTcpEcho_64B(benchmark::State& state) {
    g_asioAsyncServer.start();
    for (auto _ : state) {
        if (!runAsioAsyncEchoClient(64, state.range(0))) {
            state.SkipWithError("TCP connection failed");
            g_asioAsyncServer.stop();
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    g_asioAsyncServer.stop();
}

static void BM_AsioAsyncTcpEcho_1KB(benchmark::State& state) {
    g_asioAsyncServer.start();
    for (auto _ : state) {
        if (!runAsioAsyncEchoClient(1024, state.range(0))) {
            state.SkipWithError("TCP connection failed");
            g_asioAsyncServer.stop();
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    g_asioAsyncServer.stop();
}

static void BM_AsioAsyncTcpEcho_64KB(benchmark::State& state) {
    g_asioAsyncServer.start();
    for (auto _ : state) {
        if (!runAsioAsyncEchoClient(64 * 1024, state.range(0))) {
            state.SkipWithError("TCP connection failed");
            g_asioAsyncServer.stop();
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    g_asioAsyncServer.stop();
}

BENCHMARK(BM_AsioAsyncTcpEcho_64B)->Arg(100)->Arg(1000);
BENCHMARK(BM_AsioAsyncTcpEcho_1KB)->Arg(100)->Arg(1000);
BENCHMARK(BM_AsioAsyncTcpEcho_64KB)->Arg(100)->Arg(1000);

class AsioAsyncTcpFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        g_asioAsyncServer.start();
    }

    void TearDown(const benchmark::State&) override {
        g_asioAsyncServer.stop();
    }
};

BENCHMARK_DEFINE_F(AsioAsyncTcpFixture, ThroughputTest)(benchmark::State& state) {
    const std::size_t messageSize = state.range(0);
    const std::size_t iterations = 100;

    for (auto _ : state) {
        if (!runAsioAsyncEchoClient(messageSize, iterations)) {
            state.SkipWithError("TCP connection failed");
            return;
        }
    }

    state.SetBytesProcessed(state.iterations() * iterations * messageSize * 2);
}

BENCHMARK_REGISTER_F(AsioAsyncTcpFixture, ThroughputTest)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(65536);
