#include <catch_extensions.h>
#include <asyncio/mqtt/client.h>

using namespace asyncio::mqtt;

TEST_CASE("MQTT URL parsing", "[mqtt::client]") {
    SECTION("valid mqtt URL") {
        auto url = URL::parse("mqtt://localhost:1883");
        REQUIRE(url);
        REQUIRE(url->scheme == "mqtt");
        REQUIRE(url->host == "localhost");
        REQUIRE(url->port == 1883);
    }

    SECTION("valid mqtts URL") {
        auto url = URL::parse("mqtts://broker.example.com:8883");
        REQUIRE(url);
        REQUIRE(url->scheme == "mqtts");
        REQUIRE(url->host == "broker.example.com");
        REQUIRE(url->port == 8883);
    }

    SECTION("default mqtt port") {
        auto url = URL::parse("mqtt://localhost");
        REQUIRE(url);
        REQUIRE(url->port == DEFAULT_PORT);
    }

    SECTION("default mqtts port") {
        auto url = URL::parse("mqtts://localhost");
        REQUIRE(url);
        REQUIRE(url->port == DEFAULT_TLS_PORT);
    }

    SECTION("URL with path") {
        auto url = URL::parse("mqtt://localhost:1883/mqtt");
        REQUIRE(url);
        REQUIRE(url->host == "localhost");
        REQUIRE(url->port == 1883);
        REQUIRE(url->path == "/mqtt");
    }

    SECTION("invalid URLs") {
        REQUIRE_FALSE(URL::parse("http://localhost:1883"));  // Wrong scheme
        REQUIRE_FALSE(URL::parse("mqtt://"));  // Missing host
        REQUIRE_FALSE(URL::parse("invalid"));  // Not a URL
    }
}

TEST_CASE("MQTT Config defaults", "[mqtt::client]") {
    Config config;

    REQUIRE(config.clientId.empty());
    REQUIRE(config.username.empty());
    REQUIRE(config.password.empty());
    REQUIRE_FALSE(config.will.has_value());
    REQUIRE(config.cleanSession == true);
    REQUIRE(config.keepAlive == std::chrono::seconds(60));
    REQUIRE(config.protocolVersion == ProtocolVersion::V3_1_1);
    REQUIRE(config.connectTimeout == std::chrono::milliseconds(30000));
}

TEST_CASE("MQTT Message", "[mqtt::client]") {
    Message msg;
    msg.topic = "test/topic";
    msg.payload = {std::byte{'H'}, std::byte{'i'}};
    msg.qos = QoS::AT_LEAST_ONCE;
    msg.retain = false;

    REQUIRE(msg.topic == "test/topic");
    REQUIRE(msg.payloadString() == "Hi");
    REQUIRE(msg.qos == QoS::AT_LEAST_ONCE);
}

TEST_CASE("MQTT Subscription", "[mqtt::client]") {
    SECTION("simple subscription") {
        Subscription sub("sensors/#", QoS::AT_LEAST_ONCE);
        REQUIRE(sub.topicFilter == "sensors/#");
        REQUIRE(sub.options.maxQos == QoS::AT_LEAST_ONCE);
    }

    SECTION("subscription with options") {
        SubscribeOptions opts;
        opts.maxQos = QoS::EXACTLY_ONCE;
        opts.noLocal = true;
        opts.retainAsPublished = true;

        Subscription sub("commands/+", opts);
        REQUIRE(sub.topicFilter == "commands/+");
        REQUIRE(sub.options.maxQos == QoS::EXACTLY_ONCE);
        REQUIRE(sub.options.noLocal == true);
        REQUIRE(sub.options.retainAsPublished == true);
    }
}

TEST_CASE("MQTT Will", "[mqtt::client]") {
    Will will;
    will.topic = "status/offline";
    will.payload = {std::byte{'o'}, std::byte{'f'}, std::byte{'f'}};
    will.qos = QoS::AT_LEAST_ONCE;
    will.retain = true;

    REQUIRE(will.topic == "status/offline");
    REQUIRE(will.payload.size() == 3);
    REQUIRE(will.qos == QoS::AT_LEAST_ONCE);
    REQUIRE(will.retain == true);
}

TEST_CASE("MQTT QoS enum", "[mqtt::client]") {
    REQUIRE(static_cast<std::uint8_t>(QoS::AT_MOST_ONCE) == 0);
    REQUIRE(static_cast<std::uint8_t>(QoS::AT_LEAST_ONCE) == 1);
    REQUIRE(static_cast<std::uint8_t>(QoS::EXACTLY_ONCE) == 2);
}

TEST_CASE("MQTT ProtocolVersion enum", "[mqtt::client]") {
    REQUIRE(static_cast<std::uint8_t>(ProtocolVersion::V3_1_1) == 4);
    REQUIRE(static_cast<std::uint8_t>(ProtocolVersion::V5) == 5);
}

TEST_CASE("MQTT ReasonCode", "[mqtt::client]") {
    SECTION("success codes") {
        REQUIRE(static_cast<std::uint8_t>(ReasonCode::SUCCESS) == 0x00);
        REQUIRE(static_cast<std::uint8_t>(ReasonCode::GRANTED_QOS_0) == 0x00);
        REQUIRE(static_cast<std::uint8_t>(ReasonCode::GRANTED_QOS_1) == 0x01);
        REQUIRE(static_cast<std::uint8_t>(ReasonCode::GRANTED_QOS_2) == 0x02);
    }

    SECTION("error codes") {
        REQUIRE(static_cast<std::uint8_t>(ReasonCode::UNSPECIFIED_ERROR) == 0x80);
        REQUIRE(static_cast<std::uint8_t>(ReasonCode::NOT_AUTHORIZED) == 0x87);
    }

    SECTION("error code conversion") {
        auto ec = make_error_code(ReasonCode::NOT_AUTHORIZED);
        REQUIRE(ec.category().name() == std::string("asyncio::mqtt::ReasonCode"));
        REQUIRE(!ec.message().empty());
    }
}

TEST_CASE("MQTT Client error codes", "[mqtt::client]") {
    SECTION("error categories") {
        auto ec1 = make_error_code(Client::Error::NOT_CONNECTED);
        REQUIRE(ec1.category().name() == std::string("asyncio::mqtt::Client"));
        REQUIRE(!ec1.message().empty());

        auto ec2 = make_error_code(Client::Error::INVALID_TOPIC);
        REQUIRE(!ec2.message().empty());

        auto ec3 = make_error_code(Client::Error::PROTOCOL_ERROR);
        REQUIRE(!ec3.message().empty());
    }

    SECTION("all error codes have messages") {
        std::vector<Client::Error> errors = {
            Client::Error::INVALID_URL,
            Client::Error::UNSUPPORTED_SCHEME,
            Client::Error::CONNECTION_REFUSED,
            Client::Error::PROTOCOL_ERROR,
            Client::Error::UNEXPECTED_PACKET,
            Client::Error::PACKET_TOO_LARGE,
            Client::Error::TIMEOUT,
            Client::Error::NOT_CONNECTED,
            Client::Error::INVALID_TOPIC,
            Client::Error::INVALID_QOS,
            Client::Error::PACKET_ID_EXHAUSTED,
            Client::Error::SESSION_EXPIRED,
            Client::Error::ALREADY_CONNECTED
        };

        for (auto err : errors) {
            auto ec = make_error_code(err);
            REQUIRE(!ec.message().empty());
        }
    }
}

TEST_CASE("MQTT TLSConfig defaults", "[mqtt::client]") {
    TLSConfig config;

    REQUIRE(config.certFile.empty());
    REQUIRE(config.keyFile.empty());
    REQUIRE(config.caFile.empty());
    REQUIRE(config.verifyPeer == true);
}

TEST_CASE("MQTT SubscribeOptions defaults", "[mqtt::client]") {
    SubscribeOptions opts;

    REQUIRE(opts.maxQos == QoS::EXACTLY_ONCE);
    REQUIRE(opts.noLocal == false);
    REQUIRE(opts.retainAsPublished == false);
    REQUIRE(opts.retainHandling == SubscribeOptions::RetainHandling::SEND_ON_SUBSCRIBE);
}

TEST_CASE("MQTT constants", "[mqtt::client]") {
    REQUIRE(DEFAULT_PORT == 1883);
    REQUIRE(DEFAULT_TLS_PORT == 8883);
}
