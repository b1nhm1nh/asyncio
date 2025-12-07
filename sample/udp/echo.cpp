// Low-Latency UDP Echo Example
// Demonstrates the asyncio::net::lowlatency::UdpTransport

#include <asyncio/net/lowlatency/udp.h>
#include <iostream>
#include <csignal>
#include <atomic>

using namespace asyncio::net::lowlatency;

std::atomic<bool> running{true};

void signalHandler(int) {
    running = false;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    uint16_t port = 12345;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    std::cout << "UDP Echo Server listening on port " << port << std::endl;

    // Create low-latency UDP transport
    auto transport = create_low_latency_transport();
    transport.bind(port);

    // Echo received packets back to sender
    transport.on_receive([&](const void* data, size_t len, const Endpoint& sender) {
        std::cout << "Received " << len << " bytes from " << sender.to_string() << std::endl;
        transport.send_to(data, len, sender);
    });

    transport.start();

    std::cout << "Press Ctrl+C to stop..." << std::endl;
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    transport.stop();
    std::cout << "Stopped." << std::endl;

    return 0;
}
