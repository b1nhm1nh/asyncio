// KCP Echo Example - Reliable UDP with ARQ protocol
// Demonstrates KcpTransport for reliable message delivery

#include <asyncio/net/lowlatency/kcp.h>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>

using namespace asyncio::net::lowlatency::kcp;

std::atomic<bool> running{true};

void signal_handler(int) {
    running = false;
}

void run_server(uint16_t port) {
    std::cout << "Starting KCP echo server on port " << port << "\n";

    Config config;
    config.mode = Mode::TURBO;  // Low latency mode

    KcpTransport transport(config);
    transport.bind(port);

    std::atomic<size_t> msg_count{0};
    auto start_time = std::chrono::steady_clock::now();

    transport.on_receive([&](const void* data, size_t len, const Endpoint& sender) {
        // Echo back
        transport.send(sender, data, len);
        msg_count++;

        if (msg_count % 1000 == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed > 0) {
                std::cout << "Echoed " << msg_count << " messages, "
                          << (msg_count * 1000 / elapsed) << " msg/s\n";
            }
        }
    });

    transport.start();
    std::cout << "Server running. Press Ctrl+C to stop.\n";

    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    transport.stop();
    std::cout << "Server stopped. Total messages: " << msg_count << "\n";
}

void run_client(const std::string& host, uint16_t port, size_t count) {
    std::cout << "Connecting to KCP server at " << host << ":" << port << "\n";

    Config config;
    config.mode = Mode::TURBO;

    KcpTransport transport(config);
    transport.bind(0);  // Bind to any available port

    Endpoint server{host, port};
    transport.connect(server);

    std::atomic<size_t> received{0};
    uint64_t total_latency_us = 0;

    transport.on_receive([&](const void* data, size_t len, const Endpoint& /*sender*/) {
        // Extract timestamp from message
        if (len >= sizeof(uint64_t)) {
            uint64_t sent_ts;
            std::memcpy(&sent_ts, data, sizeof(sent_ts));
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            total_latency_us += (now - sent_ts) / 1000;  // Convert ns to us
        }
        received++;
    });

    transport.start();

    auto start = std::chrono::steady_clock::now();

    // Send messages
    for (size_t i = 0; i < count && running; i++) {
        // Message: timestamp + sequence number
        struct {
            uint64_t timestamp;
            uint64_t sequence;
        } msg;
        msg.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        msg.sequence = i;

        transport.send(server, &msg, sizeof(msg));

        // Small delay to avoid overwhelming the network
        if (i % 100 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    // Wait for responses
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (received < count && std::chrono::steady_clock::now() < deadline && running) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    transport.stop();

    std::cout << "\n=== Results ===\n";
    std::cout << "Sent: " << count << " messages\n";
    std::cout << "Received: " << received << " (" << (received * 100 / count) << "%)\n";
    std::cout << "Time: " << elapsed << " ms\n";
    if (received > 0) {
        std::cout << "Avg RTT: " << (total_latency_us / received) << " us\n";
        std::cout << "Throughput: " << (received * 1000 / std::max(1L, elapsed)) << " msg/s\n";
    }
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (argc < 2) {
        std::cout << "KCP Echo Example\n\n";
        std::cout << "Usage:\n";
        std::cout << "  Server: " << argv[0] << " server <port>\n";
        std::cout << "  Client: " << argv[0] << " client <host> <port> [count]\n\n";
        std::cout << "Example:\n";
        std::cout << "  Terminal 1: " << argv[0] << " server 12345\n";
        std::cout << "  Terminal 2: " << argv[0] << " client 127.0.0.1 12345 10000\n";
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "server") {
        uint16_t port = (argc >= 3) ? static_cast<uint16_t>(std::stoi(argv[2])) : 12345;
        run_server(port);
    } else if (mode == "client") {
        if (argc < 4) {
            std::cerr << "Client requires host and port\n";
            return 1;
        }
        std::string host = argv[2];
        uint16_t port = static_cast<uint16_t>(std::stoi(argv[3]));
        size_t count = (argc >= 5) ? static_cast<size_t>(std::stoll(argv[4])) : 1000;
        run_client(host, port, count);
    } else {
        std::cerr << "Unknown mode: " << mode << "\n";
        return 1;
    }

    return 0;
}
