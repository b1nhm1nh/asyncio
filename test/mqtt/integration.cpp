#include <catch_extensions.h>
#include <asyncio/mqtt/client.h>
#include <asyncio/time.h>
#include <random>

using namespace asyncio::mqtt;

// EMQX Public MQTT Broker: https://www.emqx.com/en/mqtt/public-mqtt5-broker
// Host: broker.emqx.io
// TCP Port: 1883
// TLS Port: 8883
constexpr auto EMQX_HOST = "broker.emqx.io";
constexpr std::uint16_t EMQX_PORT = 1883;
constexpr std::uint16_t EMQX_TLS_PORT = 8883;

static std::string generateClientId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    return fmt::format("asyncio-test-{}", dis(gen));
}

static std::string generateTopic() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(100000, 999999);
    return fmt::format("asyncio/integration-test/{}", dis(gen));
}

TEST_CASE("MQTT integration: connect to EMQX public broker", "[mqtt::integration][.network]") {
    asyncio::run([]() -> asyncio::task::Task<void> {
        Config config;
        config.clientId = generateClientId();
        config.cleanSession = true;
        config.keepAlive = std::chrono::seconds(30);

        auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_PORT, config);
        REQUIRE(clientResult.has_value());

        auto& client = *clientResult;
        REQUIRE(client.isConnected());
        REQUIRE(client.clientId() == config.clientId);

        auto disconnectResult = co_await client.disconnect();
        REQUIRE(disconnectResult.has_value());
    });
}

TEST_CASE("MQTT integration: connect with TLS", "[mqtt::integration][.network]") {
    asyncio::run([]() -> asyncio::task::Task<void> {
        Config config;
        config.clientId = generateClientId();
        config.cleanSession = true;

        TLSConfig tlsConfig;
        tlsConfig.verifyPeer = true;

        auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_TLS_PORT, config, tlsConfig);
        REQUIRE(clientResult.has_value());

        auto& client = *clientResult;
        REQUIRE(client.isConnected());

        co_await client.disconnect();
    });
}

TEST_CASE("MQTT integration: publish and subscribe QoS 0", "[mqtt::integration][.network]") {
    asyncio::run([]() -> asyncio::task::Task<void> {
        Config config;
        config.clientId = generateClientId();
        config.cleanSession = true;

        auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_PORT, config);
        REQUIRE(clientResult.has_value());
        auto& client = *clientResult;

        const auto topic = generateTopic();
        const std::string payload = "Hello MQTT QoS 0!";

        // Subscribe first
        auto subResult = co_await client.subscribe(topic, QoS::AT_MOST_ONCE);
        REQUIRE(subResult.has_value());
        REQUIRE(subResult->grantedQos == QoS::AT_MOST_ONCE);

        // Publish
        auto pubResult = co_await client.publish(topic, payload, QoS::AT_MOST_ONCE);
        REQUIRE(pubResult.has_value());

        // Receive (with timeout)
        auto timeoutResult = co_await asyncio::timeout(client.receive(), std::chrono::seconds(5));
        REQUIRE(timeoutResult.has_value());  // No timeout
        auto& msgResult = *timeoutResult;
        REQUIRE(msgResult.has_value());      // No error
        REQUIRE(msgResult->topic == topic);
        REQUIRE(msgResult->payloadString() == payload);

        co_await client.disconnect();
    });
}

TEST_CASE("MQTT integration: publish and subscribe QoS 1", "[mqtt::integration][.network]") {
    asyncio::run([]() -> asyncio::task::Task<void> {
        Config config;
        config.clientId = generateClientId();
        config.cleanSession = true;

        auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_PORT, config);
        REQUIRE(clientResult.has_value());
        auto& client = *clientResult;

        const auto topic = generateTopic();
        const std::string payload = "Hello MQTT QoS 1!";

        // Subscribe with QoS 1
        auto subResult = co_await client.subscribe(topic, QoS::AT_LEAST_ONCE);
        REQUIRE(subResult.has_value());
        REQUIRE(subResult->grantedQos == QoS::AT_LEAST_ONCE);

        // Publish with QoS 1
        auto pubResult = co_await client.publish(topic, payload, QoS::AT_LEAST_ONCE);
        REQUIRE(pubResult.has_value());
        REQUIRE(*pubResult > 0);  // QoS 1 returns packet ID

        // Receive
        auto timeoutResult = co_await asyncio::timeout(client.receive(), std::chrono::seconds(5));
        REQUIRE(timeoutResult.has_value());
        auto& msgResult = *timeoutResult;
        REQUIRE(msgResult.has_value());
        REQUIRE(msgResult->topic == topic);
        REQUIRE(msgResult->payloadString() == payload);
        REQUIRE(msgResult->qos == QoS::AT_LEAST_ONCE);

        co_await client.disconnect();
    });
}

TEST_CASE("MQTT integration: publish and subscribe QoS 2", "[mqtt::integration][.network]") {
    asyncio::run([]() -> asyncio::task::Task<void> {
        Config config;
        config.clientId = generateClientId();
        config.cleanSession = true;

        auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_PORT, config);
        REQUIRE(clientResult.has_value());
        auto& client = *clientResult;

        const auto topic = generateTopic();
        const std::string payload = "Hello MQTT QoS 2!";

        // Subscribe with QoS 2
        auto subResult = co_await client.subscribe(topic, QoS::EXACTLY_ONCE);
        REQUIRE(subResult.has_value());
        REQUIRE(subResult->grantedQos == QoS::EXACTLY_ONCE);

        // Publish with QoS 2
        auto pubResult = co_await client.publish(topic, payload, QoS::EXACTLY_ONCE);
        REQUIRE(pubResult.has_value());
        REQUIRE(*pubResult > 0);

        // Receive
        auto timeoutResult = co_await asyncio::timeout(client.receive(), std::chrono::seconds(5));
        REQUIRE(timeoutResult.has_value());
        auto& msgResult = *timeoutResult;
        REQUIRE(msgResult.has_value());
        REQUIRE(msgResult->topic == topic);
        REQUIRE(msgResult->payloadString() == payload);
        REQUIRE(msgResult->qos == QoS::EXACTLY_ONCE);

        co_await client.disconnect();
    });
}

TEST_CASE("MQTT integration: multiple subscriptions", "[mqtt::integration][.network]") {
    asyncio::run([]() -> asyncio::task::Task<void> {
        Config config;
        config.clientId = generateClientId();
        config.cleanSession = true;

        auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_PORT, config);
        REQUIRE(clientResult.has_value());
        auto& client = *clientResult;

        const auto baseTopic = generateTopic();
        const auto topic1 = baseTopic + "/a";
        const auto topic2 = baseTopic + "/b";

        // Subscribe to multiple topics
        std::vector<Subscription> subs;
        subs.emplace_back(topic1, QoS::AT_LEAST_ONCE);
        subs.emplace_back(topic2, QoS::AT_LEAST_ONCE);

        auto subResult = co_await client.subscribe(std::move(subs));
        REQUIRE(subResult.has_value());
        REQUIRE(subResult->size() == 2);

        // Publish to both topics
        co_await client.publish(topic1, "Message A", QoS::AT_LEAST_ONCE);
        co_await client.publish(topic2, "Message B", QoS::AT_LEAST_ONCE);

        // Receive both messages
        std::set<std::string> receivedTopics;
        for (int i = 0; i < 2; ++i) {
            auto timeoutResult = co_await asyncio::timeout(client.receive(), std::chrono::seconds(5));
            REQUIRE(timeoutResult.has_value());
            auto& msgResult = *timeoutResult;
            REQUIRE(msgResult.has_value());
            receivedTopics.insert(msgResult->topic);
        }

        REQUIRE(receivedTopics.count(topic1) == 1);
        REQUIRE(receivedTopics.count(topic2) == 1);

        co_await client.disconnect();
    });
}

TEST_CASE("MQTT integration: wildcard subscription", "[mqtt::integration][.network]") {
    asyncio::run([]() -> asyncio::task::Task<void> {
        Config config;
        config.clientId = generateClientId();
        config.cleanSession = true;

        auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_PORT, config);
        REQUIRE(clientResult.has_value());
        auto& client = *clientResult;

        const auto baseTopic = generateTopic();
        const auto wildcardFilter = baseTopic + "/#";
        const auto publishTopic = baseTopic + "/sensors/temp";

        // Subscribe with wildcard
        auto subResult = co_await client.subscribe(wildcardFilter, QoS::AT_LEAST_ONCE);
        REQUIRE(subResult.has_value());

        // Publish to a matching topic
        co_await client.publish(publishTopic, "temp=25.5", QoS::AT_LEAST_ONCE);

        // Should receive the message
        auto timeoutResult = co_await asyncio::timeout(client.receive(), std::chrono::seconds(5));
        REQUIRE(timeoutResult.has_value());
        auto& msgResult = *timeoutResult;
        REQUIRE(msgResult.has_value());
        REQUIRE(msgResult->topic == publishTopic);

        co_await client.disconnect();
    });
}

TEST_CASE("MQTT integration: unsubscribe", "[mqtt::integration][.network]") {
    asyncio::run([]() -> asyncio::task::Task<void> {
        Config config;
        config.clientId = generateClientId();
        config.cleanSession = true;

        auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_PORT, config);
        REQUIRE(clientResult.has_value());
        auto& client = *clientResult;

        const auto topic = generateTopic();

        // Subscribe
        auto subResult = co_await client.subscribe(topic, QoS::AT_LEAST_ONCE);
        REQUIRE(subResult.has_value());

        // Unsubscribe
        auto unsubResult = co_await client.unsubscribe(topic);
        REQUIRE(unsubResult.has_value());

        co_await client.disconnect();
    });
}

TEST_CASE("MQTT integration: binary payload", "[mqtt::integration][.network]") {
    asyncio::run([]() -> asyncio::task::Task<void> {
        Config config;
        config.clientId = generateClientId();
        config.cleanSession = true;

        auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_PORT, config);
        REQUIRE(clientResult.has_value());
        auto& client = *clientResult;

        const auto topic = generateTopic();

        // Create binary payload
        std::vector<std::byte> payload = {
            std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
            std::byte{0xFF}, std::byte{0xFE}, std::byte{0xFD}
        };

        // Subscribe
        auto subResult = co_await client.subscribe(topic, QoS::AT_LEAST_ONCE);
        REQUIRE(subResult.has_value());

        // Publish binary
        auto pubResult = co_await client.publish(
            topic, std::span<const std::byte>(payload), QoS::AT_LEAST_ONCE);
        REQUIRE(pubResult.has_value());

        // Receive
        auto timeoutResult = co_await asyncio::timeout(client.receive(), std::chrono::seconds(5));
        REQUIRE(timeoutResult.has_value());
        auto& msgResult = *timeoutResult;
        REQUIRE(msgResult.has_value());
        REQUIRE(msgResult->payload.size() == payload.size());
        REQUIRE(std::equal(msgResult->payload.begin(), msgResult->payload.end(), payload.begin()));

        co_await client.disconnect();
    });
}

TEST_CASE("MQTT integration: retain message", "[mqtt::integration][.network]") {
    asyncio::run([]() -> asyncio::task::Task<void> {
        const auto topic = generateTopic();
        const std::string payload = "Retained message";

        // First client: publish retained message
        {
            Config config;
            config.clientId = generateClientId();
            config.cleanSession = true;

            auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_PORT, config);
            REQUIRE(clientResult.has_value());
            auto& client = *clientResult;

            // Publish with retain flag
            auto pubResult = co_await client.publish(topic, payload, QoS::AT_LEAST_ONCE, true);
            REQUIRE(pubResult.has_value());

            co_await client.disconnect();
        }

        // Small delay to ensure message is retained
        co_await asyncio::sleep(std::chrono::milliseconds(500));

        // Second client: should receive retained message on subscribe
        {
            Config config;
            config.clientId = generateClientId();
            config.cleanSession = true;

            auto clientResult = co_await Client::connect(EMQX_HOST, EMQX_PORT, config);
            REQUIRE(clientResult.has_value());
            auto& client = *clientResult;

            // Subscribe - should receive retained message
            auto subResult = co_await client.subscribe(topic, QoS::AT_LEAST_ONCE);
            REQUIRE(subResult.has_value());

            // Receive retained message
            auto timeoutResult = co_await asyncio::timeout(client.receive(), std::chrono::seconds(5));
            REQUIRE(timeoutResult.has_value());
            auto& msgResult = *timeoutResult;
            REQUIRE(msgResult.has_value());
            REQUIRE(msgResult->topic == topic);
            REQUIRE(msgResult->payloadString() == payload);
            REQUIRE(msgResult->retain == true);

            // Clean up: publish empty retained message to clear it
            co_await client.publish(topic, "", QoS::AT_LEAST_ONCE, true);

            co_await client.disconnect();
        }
    });
}
