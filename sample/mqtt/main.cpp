#include <asyncio/mqtt/client.h>
#include <asyncio/time.h>
#include <zero/cmdline.h>
#include <fmt/std.h>

using namespace asyncio::mqtt;

asyncio::task::Task<void, std::error_code> asyncMain(const int argc, char *argv[]) {

    zero::Cmdline cmdline;

    cmdline.add<std::string>("host", "MQTT broker host");
    cmdline.add<std::uint16_t>("port", "MQTT broker port");
    cmdline.add<std::string>("topic", "Topic to subscribe/publish");

    cmdline.parse(argc, argv);

    const auto host = cmdline.get<std::string>("host");
    const auto port = cmdline.get<std::uint16_t>("port");
    const auto topic = cmdline.get<std::string>("topic");

    // Configure client
    Config config;
    config.clientId = fmt::format("asyncio-mqtt-{}", std::rand());
    config.cleanSession = true;
    config.keepAlive = std::chrono::seconds(60);

    fmt::print("Connecting to {}:{}...\n", host, port);
    fmt::print("  Client ID: {}\n", config.clientId);
    fmt::print("  Clean Session: {}\n", config.cleanSession);
    fmt::print("  Keep Alive: {} seconds\n", config.keepAlive.count());
    fmt::print("  Protocol Version: {}\n", static_cast<int>(config.protocolVersion));

    // Connect to broker
    auto client = co_await Client::connect(host, port, config);
    if (!client) {
        fmt::print("Connection failed: {}\n", client.error().message());
        co_return std::unexpected(client.error());
    }

    fmt::print("Connected! Client ID: {}\n", client->clientId());

    // Subscribe to topic
    fmt::print("Subscribing to '{}'...\n", topic);
    auto subResult = co_await client->subscribe(topic, QoS::AT_LEAST_ONCE);
    CO_EXPECT(subResult);
    fmt::print("Subscribed with QoS {}\n", static_cast<int>(subResult->grantedQos));

    // Publish a test message
    const std::string payload = fmt::format("Hello from asyncio MQTT at {}",
        std::chrono::system_clock::now());
    fmt::print("Publishing: '{}'\n", payload);
    auto pubResult = co_await client->publish(topic, payload, QoS::AT_LEAST_ONCE);
    CO_EXPECT(pubResult);
    fmt::print("Published with packet ID: {}\n", *pubResult);

    // Receive messages (with timeout)
    fmt::print("Waiting for messages (10 seconds)...\n");

    auto timeoutResult = co_await asyncio::timeout(client->receive(), std::chrono::seconds(10));

    if (timeoutResult) {
        auto& msgResult = *timeoutResult;
        if (msgResult) {
            auto& msg = *msgResult;
            fmt::print("Received message:\n");
            fmt::print("  Topic: {}\n", msg.topic);
            fmt::print("  Payload: {}\n", msg.payloadString());
            fmt::print("  QoS: {}\n", static_cast<int>(msg.qos));
            fmt::print("  Retain: {}\n", msg.retain);
        } else {
            fmt::print("Receive error: {}\n", msgResult.error().message());
        }
    } else {
        fmt::print("Timeout waiting for message\n");
    }

    // Disconnect
    fmt::print("Disconnecting...\n");
    CO_EXPECT(co_await client->disconnect());
    fmt::print("Disconnected.\n");

    co_return {};
}
