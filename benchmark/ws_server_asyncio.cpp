#include "common.h"
#include <benchmark/benchmark.h>
#include <asyncio/net/stream.h>
#include <asyncio/http/websocket.h>
#include <asyncio/event_loop.h>
#include <asyncio/buffer.h>
#include <asyncio/binary.h>
#include <thread>
#include <atomic>
#include <openssl/sha.h>
#include <zero/encoding/base64.h>

namespace {
    constexpr auto MASKING_KEY_LENGTH = 4;
    constexpr auto TWO_BYTE_PAYLOAD_LENGTH = 126;
    constexpr auto EIGHT_BYTE_PAYLOAD_LENGTH = 127;
    constexpr auto MAX_SINGLE_BYTE_PAYLOAD_LENGTH = 125;
    constexpr auto MAX_TWO_BYTE_PAYLOAD_LENGTH = 65535;
    constexpr auto WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    constexpr auto OPCODE_MASK = std::byte{0x0f};
    constexpr auto FINAL_BIT = std::byte{0x80};
    constexpr auto LENGTH_MASK = std::byte{0x7f};
    constexpr auto MASK_BIT = std::byte{0x80};

    enum class Opcode : std::uint8_t {
        CONTINUATION = 0,
        TEXT = 1,
        BINARY = 2,
        CLOSE = 8,
        PING = 9,
        PONG = 10
    };

    struct Frame {
        Opcode opcode;
        bool fin;
        std::vector<std::byte> data;
    };

    // Pre-allocated buffer for frame operations (thread-local for server)
    // Avoids allocation in hot path for messages up to 64KB
    constexpr std::size_t MAX_PREALLOCATED_SIZE = 64 * 1024 + 14; // max payload + max header
    thread_local std::vector<std::byte> g_writeBuffer(MAX_PREALLOCATED_SIZE);

    // Optimized XOR unmasking - process 8 bytes at a time
    inline void unmaskData(std::span<std::byte> data, const std::array<std::byte, 4>& maskKey) {
        const std::size_t length = data.size();
        if (length == 0) return;

        // Create 8-byte mask by repeating 4-byte key
        std::uint64_t mask64;
        std::memcpy(&mask64, maskKey.data(), 4);
        mask64 |= (mask64 << 32);

        // Process 8 bytes at a time
        std::size_t i = 0;
        auto* ptr = reinterpret_cast<std::uint64_t*>(data.data());
        const std::size_t chunks = length / 8;
        for (std::size_t c = 0; c < chunks; ++c, ++ptr) {
            *ptr ^= mask64;
        }
        i = chunks * 8;

        // Handle remaining bytes
        for (; i < length; ++i) {
            data[i] ^= maskKey[i % 4];
        }
    }

    // Simple WebSocket frame reader for server (expects masked frames)
    template<typename Reader>
    asyncio::task::Task<Frame, std::error_code> readFrame(Reader& reader) {
        std::array<std::byte, 2> header{};
        CO_EXPECT(co_await reader.readExactly(header));

        Frame frame;
        frame.opcode = static_cast<Opcode>(std::to_integer<uint8_t>(header[0] & OPCODE_MASK));
        frame.fin = std::to_integer<bool>(header[0] & FINAL_BIT);
        bool masked = std::to_integer<bool>(header[1] & MASK_BIT);
        std::size_t length = std::to_integer<std::size_t>(header[1] & LENGTH_MASK);

        if (length == TWO_BYTE_PAYLOAD_LENGTH) {
            std::array<std::byte, 2> lenBytes{};
            CO_EXPECT(co_await reader.readExactly(lenBytes));
            length = (std::to_integer<std::size_t>(lenBytes[0]) << 8) |
                     std::to_integer<std::size_t>(lenBytes[1]);
        } else if (length == EIGHT_BYTE_PAYLOAD_LENGTH) {
            std::array<std::byte, 8> lenBytes{};
            CO_EXPECT(co_await reader.readExactly(lenBytes));
            length = 0;
            for (int i = 0; i < 8; ++i) {
                length = (length << 8) | std::to_integer<std::size_t>(lenBytes[i]);
            }
        }

        std::array<std::byte, MASKING_KEY_LENGTH> maskKey{};
        if (masked) {
            CO_EXPECT(co_await reader.readExactly(maskKey));
        }

        frame.data.resize(length);
        CO_EXPECT(co_await reader.readExactly(frame.data));

        if (masked) {
            unmaskData(frame.data, maskKey);
        }

        co_return frame;
    }

    // Write frame (server sends unmasked) - optimized single write
    asyncio::task::Task<void, std::error_code> writeFrame(
        asyncio::IWriter& writer,
        Opcode opcode,
        std::span<const std::byte> data,
        bool fin = true
    ) {
        const std::size_t length = data.size();
        std::size_t headerSize = 2;

        // Calculate header size
        if (length > MAX_TWO_BYTE_PAYLOAD_LENGTH) {
            headerSize = 10; // 2 + 8
        } else if (length > MAX_SINGLE_BYTE_PAYLOAD_LENGTH) {
            headerSize = 4;  // 2 + 2
        }

        // Use pre-allocated buffer for combined header + data write
        const std::size_t totalSize = headerSize + length;
        if (totalSize <= MAX_PREALLOCATED_SIZE) {
            // Fast path: single write with pre-allocated buffer
            auto& buffer = g_writeBuffer;

            // Build header
            buffer[0] = static_cast<std::byte>(opcode) | (fin ? FINAL_BIT : std::byte{0});

            if (length > MAX_TWO_BYTE_PAYLOAD_LENGTH) {
                buffer[1] = std::byte{EIGHT_BYTE_PAYLOAD_LENGTH};
                for (int i = 0; i < 8; ++i) {
                    buffer[2 + i] = static_cast<std::byte>((length >> (56 - i * 8)) & 0xFF);
                }
            } else if (length > MAX_SINGLE_BYTE_PAYLOAD_LENGTH) {
                buffer[1] = std::byte{TWO_BYTE_PAYLOAD_LENGTH};
                buffer[2] = static_cast<std::byte>((length >> 8) & 0xFF);
                buffer[3] = static_cast<std::byte>(length & 0xFF);
            } else {
                buffer[1] = static_cast<std::byte>(length);
            }

            // Copy payload
            std::memcpy(buffer.data() + headerSize, data.data(), length);

            // Single write
            CO_EXPECT(co_await writer.writeAll({buffer.data(), totalSize}));
        } else {
            // Fallback for very large frames: multiple writes
            std::array<std::byte, 10> header{};
            header[0] = static_cast<std::byte>(opcode) | (fin ? FINAL_BIT : std::byte{0});

            if (length > MAX_TWO_BYTE_PAYLOAD_LENGTH) {
                header[1] = std::byte{EIGHT_BYTE_PAYLOAD_LENGTH};
                for (int i = 0; i < 8; ++i) {
                    header[2 + i] = static_cast<std::byte>((length >> (56 - i * 8)) & 0xFF);
                }
            } else if (length > MAX_SINGLE_BYTE_PAYLOAD_LENGTH) {
                header[1] = std::byte{TWO_BYTE_PAYLOAD_LENGTH};
                header[2] = static_cast<std::byte>((length >> 8) & 0xFF);
                header[3] = static_cast<std::byte>(length & 0xFF);
            } else {
                header[1] = static_cast<std::byte>(length);
            }

            CO_EXPECT(co_await writer.writeAll({header.data(), headerSize}));
            CO_EXPECT(co_await writer.writeAll(data));
        }

        co_return {};
    }

    // Handle WebSocket upgrade
    asyncio::task::Task<bool, std::error_code> handleUpgrade(
        asyncio::BufReader<std::shared_ptr<asyncio::net::TCPStream>>& reader,
        asyncio::net::TCPStream& stream
    ) {
        std::string wsKey;

        // Read HTTP request
        while (true) {
            auto line = co_await reader.readLine();
            CO_EXPECT(line);

            if (line->empty())
                break;

            if (line->starts_with("Sec-WebSocket-Key: ")) {
                wsKey = line->substr(19);
            }
        }

        if (wsKey.empty()) {
            co_return false;
        }

        // Calculate accept key
        std::array<std::byte, SHA_DIGEST_LENGTH> digest{};
        std::string data = wsKey + WS_MAGIC;
        SHA1(
            reinterpret_cast<const unsigned char*>(data.data()),
            data.size(),
            reinterpret_cast<unsigned char*>(digest.data())
        );
        std::string acceptKey = zero::encoding::base64::encode(digest);

        // Send upgrade response
        std::string response = fmt::format(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: {}\r\n\r\n",
            acceptKey
        );

        CO_EXPECT(co_await stream.writeAll(std::as_bytes(std::span{response})));
        co_return true;
    }

    // Handle WebSocket echo
    asyncio::task::Task<void, std::error_code> handleWsEcho(asyncio::net::TCPStream stream) {
        // HFT optimization: disable Nagle's algorithm for lower latency
        stream.noDelay(true);

        auto streamPtr = std::make_shared<asyncio::net::TCPStream>(std::move(stream));
        asyncio::BufReader reader{streamPtr};

        auto upgraded = co_await handleUpgrade(reader, *streamPtr);
        CO_EXPECT(upgraded);

        if (!*upgraded) {
            co_return {};
        }

        while (true) {
            auto frame = co_await readFrame(reader);
            if (!frame) break;

            if (frame->opcode == Opcode::CLOSE) {
                // Echo close frame
                co_await writeFrame(*streamPtr, Opcode::CLOSE, frame->data);
                break;
            } else if (frame->opcode == Opcode::PING) {
                CO_EXPECT(co_await writeFrame(*streamPtr, Opcode::PONG, frame->data));
            } else if (frame->opcode == Opcode::TEXT || frame->opcode == Opcode::BINARY) {
                // Echo the message
                CO_EXPECT(co_await writeFrame(*streamPtr, frame->opcode, frame->data));
            }
        }

        co_await streamPtr->close();
        co_return {};
    }

    asyncio::task::Task<void, std::error_code> runWsServer(std::atomic<bool>& running) {
        auto listener = asyncio::net::TCPListener::listen(bench::LOCALHOST, bench::WS_PORT + 1);
        if (!listener)
            co_return std::unexpected{listener.error()};

        running = true;

        while (running) {
            auto stream = co_await listener->accept();
            if (!stream)
                break;

            handleWsEcho(*std::move(stream)).future().fail([](const auto&) {});
        }

        co_await listener->close();
        co_return {};
    }

    std::atomic<bool> g_serverRunning{false};
    std::thread g_serverThread;

    void startWsServer() {
        g_serverRunning = true;
        g_serverThread = std::thread([] {
            asyncio::run([&]() {
                return runWsServer(g_serverRunning);
            });
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    void stopWsServer() {
        g_serverRunning = false;
        // Connect to unblock accept
        try {
            asyncio::run([]() -> asyncio::task::Task<void, std::error_code> {
                auto stream = co_await asyncio::net::TCPStream::connect(bench::LOCALHOST, bench::WS_PORT + 1);
                if (stream)
                    co_await stream->close();
                co_return {};
            });
        } catch (...) {}

        if (g_serverThread.joinable())
            g_serverThread.join();
    }

    // Client task
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
}

// Benchmark with internal asyncio WS server
static void BM_AsyncioWsInternal_64B(benchmark::State& state) {
    startWsServer();
    const std::string url = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT + 1) + "/";

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsClient(url, 64, state.range(0));
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            stopWsServer();
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    stopWsServer();
}

static void BM_AsyncioWsInternal_1KB(benchmark::State& state) {
    startWsServer();
    const std::string url = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT + 1) + "/";

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsClient(url, 1024, state.range(0));
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            stopWsServer();
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    stopWsServer();
}

static void BM_AsyncioWsInternal_64KB(benchmark::State& state) {
    startWsServer();
    const std::string url = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT + 1) + "/";

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsClient(url, 64 * 1024, state.range(0));
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            stopWsServer();
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
    stopWsServer();
}

BENCHMARK(BM_AsyncioWsInternal_64B)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK(BM_AsyncioWsInternal_1KB)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK(BM_AsyncioWsInternal_64KB)->Arg(10)->Arg(100);

class AsyncioWsInternalFixture : public benchmark::Fixture {
public:
    void SetUp(const benchmark::State&) override {
        startWsServer();
        url_ = "http://" + std::string(bench::LOCALHOST) + ":" + std::to_string(bench::WS_PORT + 1) + "/";
    }

    void TearDown(const benchmark::State&) override {
        stopWsServer();
    }

    std::string url_;
};

BENCHMARK_DEFINE_F(AsyncioWsInternalFixture, ThroughputTest)(benchmark::State& state) {
    const std::size_t messageSize = state.range(0);
    const std::size_t iterations = 100;

    for (auto _ : state) {
        auto result = asyncio::run([&]() {
            return runWsClient(url_, messageSize, iterations);
        });

        if (!result || !*result) {
            state.SkipWithError("WebSocket connection failed");
            return;
        }
    }

    state.SetBytesProcessed(state.iterations() * iterations * messageSize * 2);
}

BENCHMARK_REGISTER_F(AsyncioWsInternalFixture, ThroughputTest)
    ->Arg(64)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(65536);
