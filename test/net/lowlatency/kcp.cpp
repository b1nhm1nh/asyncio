#include <catch_extensions.h>
#include <asyncio/net/lowlatency/kcp.h>
#include <thread>
#include <chrono>

using namespace asyncio::net::lowlatency::kcp;

TEST_CASE("KCP Endpoint", "[net::lowlatency::kcp]") {
    SECTION("construction and comparison") {
        Endpoint ep1{"127.0.0.1", 12345};
        Endpoint ep2{"127.0.0.1", 12345};
        Endpoint ep3{"127.0.0.1", 12346};

        REQUIRE(ep1 == ep2);
        REQUIRE(!(ep1 == ep3));
    }

    SECTION("to_string") {
        Endpoint ep{"192.168.1.1", 8080};
        REQUIRE(ep.to_string() == "192.168.1.1:8080");
    }

    SECTION("sockaddr conversion") {
        Endpoint ep1{"127.0.0.1", 12345};
        auto addr = ep1.to_sockaddr();
        auto ep2 = Endpoint::from_sockaddr(addr);
        REQUIRE(ep1 == ep2);
    }
}

TEST_CASE("KCP UdpSocket", "[net::lowlatency::kcp]") {
    SECTION("create and bind") {
        auto socket = UdpSocket::create();
        REQUIRE(socket.valid());
        socket.bind(0);  // Bind to any available port
    }

    SECTION("move constructor") {
        auto socket1 = UdpSocket::create();
        REQUIRE(socket1.valid());
        auto fd = socket1.fd();

        UdpSocket socket2 = std::move(socket1);
        REQUIRE(socket2.valid());
        REQUIRE(socket2.fd() == fd);
        REQUIRE(!socket1.valid());
    }
}

TEST_CASE("KCP Config", "[net::lowlatency::kcp]") {
    SECTION("default config") {
        Config config;
        REQUIRE(config.mode == Mode::TURBO);
        REQUIRE(config.conv == DEFAULT_CONV);
        REQUIRE(config.mtu == DEFAULT_MTU);
    }

    SECTION("custom config") {
        Config config;
        config.mode = Mode::FASTEST;
        config.snd_wnd = 2048;
        config.rcv_wnd = 2048;
        REQUIRE(config.snd_wnd == 2048);
    }
}

TEST_CASE("KCP Transport basic operations", "[net::lowlatency::kcp]") {
    SECTION("create and bind") {
        KcpTransport transport;
        transport.bind(0);  // Bind to any available port
        REQUIRE(!transport.running());
    }

    SECTION("start and stop") {
        KcpTransport transport;
        transport.bind(0);
        transport.start();
        REQUIRE(transport.running());
        transport.stop();
        REQUIRE(!transport.running());
    }
}

TEST_CASE("KCP Transport send/receive", "[net::lowlatency::kcp]") {
    SECTION("loopback communication") {
        // Server
        Config server_config;
        server_config.mode = Mode::TURBO;
        KcpTransport server(server_config);
        server.bind(23456);

        std::atomic<int> received_count{0};
        std::vector<char> received_data;
        std::mutex data_mutex;

        server.on_receive([&](const void* data, size_t len, const Endpoint& /*sender*/) {
            std::lock_guard lock(data_mutex);
            received_data.assign(static_cast<const char*>(data),
                               static_cast<const char*>(data) + len);
            received_count++;
        });

        server.start();

        // Client
        Config client_config;
        client_config.mode = Mode::TURBO;
        KcpTransport client(client_config);
        client.bind(0);

        Endpoint server_ep{"127.0.0.1", 23456};
        client.connect(server_ep);
        client.start();

        // Send message
        const char* message = "Hello KCP!";
        client.send(server_ep, message, strlen(message));

        // Wait for delivery (KCP has some latency due to ARQ protocol)
        for (int i = 0; i < 100 && received_count == 0; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        client.stop();
        server.stop();

        REQUIRE(received_count >= 1);
        {
            std::lock_guard lock(data_mutex);
            std::string msg(received_data.begin(), received_data.end());
            REQUIRE(msg == "Hello KCP!");
        }
    }

    SECTION("multiple messages") {
        Config config;
        config.mode = Mode::TURBO;

        KcpTransport server(config);
        server.bind(23457);

        std::atomic<int> count{0};

        server.on_receive([&](const void* /*data*/, size_t /*len*/, const Endpoint& /*sender*/) {
            count++;
        });

        server.start();

        KcpTransport client(config);
        client.bind(0);
        Endpoint server_ep{"127.0.0.1", 23457};
        client.connect(server_ep);
        client.start();

        // Send multiple messages
        for (int i = 0; i < 10; i++) {
            std::string msg = "Message " + std::to_string(i);
            client.send(server_ep, msg.data(), msg.size());
        }

        // Wait for delivery
        for (int i = 0; i < 200 && count < 10; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        client.stop();
        server.stop();

        REQUIRE(count >= 5);  // At least half should arrive (KCP is reliable)
    }
}

TEST_CASE("KCP Session configuration", "[net::lowlatency::kcp]") {
    SECTION("different modes") {
        auto output_cb = [](const char*, size_t, const Endpoint&) {};
        Endpoint remote{"127.0.0.1", 12345};

        KcpSession session(DEFAULT_CONV, remote, output_cb);

        // Test different modes don't crash
        session.configure(Mode::NORMAL);
        session.configure(Mode::FAST);
        session.configure(Mode::TURBO);
        session.configure(Mode::FASTEST);
        session.configure(Mode::RAW_UDP);
    }
}

TEST_CASE("KCP EndpointHash", "[net::lowlatency::kcp]") {
    EndpointHash hasher;

    Endpoint ep1{"127.0.0.1", 12345};
    Endpoint ep2{"127.0.0.1", 12345};
    Endpoint ep3{"127.0.0.1", 12346};

    REQUIRE(hasher(ep1) == hasher(ep2));
    REQUIRE(hasher(ep1) != hasher(ep3));
}
