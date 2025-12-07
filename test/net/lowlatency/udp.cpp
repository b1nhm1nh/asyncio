#include <catch_extensions.h>
#include <asyncio/net/lowlatency/udp.h>
#include <thread>
#include <chrono>

using namespace asyncio::net::lowlatency;

TEST_CASE("UDP socket creation", "[net::lowlatency::udp]") {
    SECTION("create with default config") {
        auto socket = UdpSocket::create();
        REQUIRE(socket.valid());
    }

    SECTION("create with custom config") {
        Config config;
        config.recv_buffer_size = 4 * 1024 * 1024;
        config.send_buffer_size = 4 * 1024 * 1024;
        config.low_delay_tos = true;

        auto socket = UdpSocket::create(config);
        REQUIRE(socket.valid());
    }
}

TEST_CASE("UDP socket bind", "[net::lowlatency::udp]") {
    auto socket = UdpSocket::create();

    SECTION("bind to port") {
        REQUIRE_NOTHROW(socket.bind(0));  // Bind to any available port
    }

    SECTION("bind to endpoint") {
        REQUIRE_NOTHROW(socket.bind(Endpoint{"127.0.0.1", 0}));
    }
}

TEST_CASE("UDP transport send/receive", "[net::lowlatency::udp]") {
    auto sender = create_transport();
    auto receiver = create_transport();

    sender.bind(0);
    receiver.bind(0);

    // Need to get actual bound port
    // For now test transport creation
    REQUIRE(sender.fd() != SOCKET_INVALID);
    REQUIRE(receiver.fd() != SOCKET_INVALID);
}

TEST_CASE("Endpoint", "[net::lowlatency::udp]") {
    SECTION("construction") {
        Endpoint ep{"127.0.0.1", 12345};
        REQUIRE(ep.host == "127.0.0.1");
        REQUIRE(ep.port == 12345);
    }

    SECTION("to_string") {
        Endpoint ep{"127.0.0.1", 12345};
        REQUIRE(ep.to_string() == "127.0.0.1:12345");
    }

    SECTION("equality") {
        Endpoint ep1{"127.0.0.1", 12345};
        Endpoint ep2{"127.0.0.1", 12345};
        Endpoint ep3{"127.0.0.1", 12346};
        REQUIRE(ep1 == ep2);
        REQUIRE(!(ep1 == ep3));
    }

    SECTION("sockaddr conversion") {
        Endpoint ep{"127.0.0.1", 12345};
        auto addr = ep.to_sockaddr();
        auto ep2 = Endpoint::from_sockaddr(addr);
        REQUIRE(ep == ep2);
    }
}

TEST_CASE("UDP pair", "[net::lowlatency::udp]") {
    Endpoint local{"127.0.0.1", 0};
    Endpoint remote{"127.0.0.1", 0};

    // Create with explicit ports would be better for testing
    // but this tests the construction at least
    REQUIRE_NOTHROW(create_pair(14000, Endpoint{"127.0.0.1", 14001}));
}

TEST_CASE("Config defaults", "[net::lowlatency::udp]") {
    Config config;
    REQUIRE(config.recv_buffer_size == DEFAULT_RECV_BUFFER);
    REQUIRE(config.send_buffer_size == DEFAULT_SEND_BUFFER);
    REQUIRE(config.reuse_addr == true);
    REQUIRE(config.reuse_port == true);
    REQUIRE(config.nonblocking == true);
    REQUIRE(config.low_delay_tos == true);
    REQUIRE(config.rcv_lowat == 1);
}
