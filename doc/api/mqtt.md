# MQTT

This module provides an async MQTT client for publish/subscribe messaging.

## Overview

The MQTT module provides a coroutine-based MQTT client supporting both MQTT 3.1.1 and MQTT 5.0 protocols. It enables asynchronous publish/subscribe messaging patterns with full QoS support.

```cpp
#include <asyncio/mqtt/client.h>

using namespace asyncio::mqtt;

task::Task<void, std::error_code> asyncMain() {
    Config config;
    config.clientId = "my-client";

    // Connect to broker
    auto client = co_await Client::connect("localhost", 1883, config);
    CO_EXPECT(client);

    // Subscribe to topic
    co_await client->subscribe("sensors/temperature", QoS::AT_LEAST_ONCE);

    // Publish message
    co_await client->publish("sensors/temperature", "25.5", QoS::AT_LEAST_ONCE);

    // Receive messages
    auto msg = co_await client->receive();
    if (msg) {
        std::cout << "Topic: " << msg->topic << ", Payload: " << msg->payloadString() << "\n";
    }

    co_await client->disconnect();
    co_return {};
}
```

## Constants

```cpp
constexpr std::uint16_t DEFAULT_PORT = 1883;      // Default MQTT port
constexpr std::uint16_t DEFAULT_TLS_PORT = 8883;  // Default MQTT over TLS port
```

## Enum `ProtocolVersion`

```cpp
enum class ProtocolVersion : std::uint8_t {
    V3_1_1 = 4,  // MQTT 3.1.1
    V5 = 5       // MQTT 5.0
};
```

Specifies the MQTT protocol version to use.

## Enum `QoS`

```cpp
enum class QoS : std::uint8_t {
    AT_MOST_ONCE = 0,   // Fire and forget (QoS 0)
    AT_LEAST_ONCE = 1,  // Acknowledged delivery (QoS 1)
    EXACTLY_ONCE = 2    // Assured delivery (QoS 2)
};
```

Quality of Service levels for message delivery.

- **AT_MOST_ONCE**: Message is delivered at most once. No acknowledgment.
- **AT_LEAST_ONCE**: Message is delivered at least once. Requires acknowledgment (PUBACK).
- **EXACTLY_ONCE**: Message is delivered exactly once. Uses 4-way handshake (PUBREC/PUBREL/PUBCOMP).

## Struct `Config`

```cpp
struct Config {
    std::string clientId;                              // Client identifier
    std::string username;                              // Username for authentication
    std::string password;                              // Password for authentication
    std::optional<Will> will;                          // Will message
    bool cleanSession = true;                          // Clean session flag
    std::chrono::seconds keepAlive{60};               // Keep-alive interval
    ProtocolVersion protocolVersion = ProtocolVersion::V3_1_1;
    std::chrono::milliseconds connectTimeout{30000};   // Connection timeout
    std::size_t maxReceiveBufferSize = 256 * 1024;    // Max buffer size
    std::uint16_t receiveMaximum = 65535;             // Max in-flight messages
};
```

Client configuration options.

### Field `clientId`

Unique identifier for the client. If empty, a random ID is generated.

### Field `cleanSession`

When `true`, the broker discards any previous session state. When `false`, the broker restores session state (subscriptions, pending messages).

### Field `keepAlive`

Maximum interval between client communications. The client sends PINGREQ if no other packets are sent within this interval.

## Struct `Will`

```cpp
struct Will {
    std::string topic;
    std::vector<std::byte> payload;
    QoS qos = QoS::AT_MOST_ONCE;
    bool retain = false;
};
```

Will message published by the broker when the client disconnects unexpectedly.

## Struct `Message`

```cpp
struct Message {
    std::string topic;
    std::vector<std::byte> payload;
    QoS qos = QoS::AT_MOST_ONCE;
    bool retain = false;
    bool dup = false;
    std::uint16_t packetId = 0;

    [[nodiscard]] std::string payloadString() const;
};
```

Represents a received PUBLISH message.

### Method `payloadString`

```cpp
[[nodiscard]] std::string payloadString() const;
```

Returns the payload as a string.

## Struct `Subscription`

```cpp
struct Subscription {
    std::string topicFilter;
    SubscribeOptions options;

    Subscription(std::string filter, QoS qos = QoS::AT_MOST_ONCE);
    Subscription(std::string filter, SubscribeOptions opts);
};
```

Represents a topic subscription with options.

### Topic Filters

Topic filters support wildcards:
- `+` matches a single level (e.g., `sensors/+/temperature`)
- `#` matches multiple levels at the end (e.g., `sensors/#`)

## Struct `TLSConfig`

```cpp
struct TLSConfig {
    std::string certFile;    // Client certificate file
    std::string keyFile;     // Client key file
    std::string caFile;      // CA certificate file
    bool verifyPeer = true;  // Verify server certificate
};
```

TLS configuration for secure connections.

## Class `Client`

The main MQTT client class.

### Error Codes

```cpp
DEFINE_ERROR_CODE_INNER(
    Error,
    "asyncio::mqtt::Client",
    INVALID_URL, "invalid mqtt url",
    UNSUPPORTED_SCHEME, "unsupported mqtt scheme",
    CONNECTION_REFUSED, "connection refused by broker",
    PROTOCOL_ERROR, "mqtt protocol error",
    UNEXPECTED_PACKET, "unexpected packet received",
    PACKET_TOO_LARGE, "packet exceeds maximum size",
    TIMEOUT, "operation timed out",
    NOT_CONNECTED, "client is not connected",
    INVALID_TOPIC, "invalid topic name or filter",
    INVALID_QOS, "invalid qos level",
    PACKET_ID_EXHAUSTED, "no available packet identifiers",
    SESSION_EXPIRED, "session has expired",
    ALREADY_CONNECTED, "client is already connected"
)
```

### Static Method `connect` (TCP)

```cpp
static task::Task<Client, std::error_code>
connect(std::string host, std::uint16_t port, Config config);
```

Connects to an MQTT broker over TCP.

```cpp
Config config;
config.clientId = "my-client";

auto client = co_await Client::connect("broker.example.com", 1883, config);
CO_EXPECT(client);
```

### Static Method `connect` (TLS)

```cpp
static task::Task<Client, std::error_code>
connect(std::string host, std::uint16_t port, Config config, TLSConfig tlsConfig);
```

Connects to an MQTT broker over TLS.

```cpp
Config config;
config.clientId = "my-client";

TLSConfig tlsConfig;
tlsConfig.caFile = "/path/to/ca.crt";

auto client = co_await Client::connect("broker.example.com", 8883, config, tlsConfig);
```

### Static Method `connect` (URL)

```cpp
static task::Task<Client, std::error_code>
connect(std::string url, Config config);
```

Connects using a URL. Supports `mqtt://` and `mqtts://` schemes.

```cpp
auto client = co_await Client::connect("mqtt://broker.example.com:1883", config);
auto secureClient = co_await Client::connect("mqtts://broker.example.com:8883", config);
```

### Method `isConnected`

```cpp
[[nodiscard]] bool isConnected() const;
```

Returns `true` if the client is connected to the broker.

### Method `clientId`

```cpp
[[nodiscard]] const std::string &clientId() const;
```

Returns the client identifier.

### Method `publish`

```cpp
task::Task<std::uint16_t, std::error_code>
publish(std::string topic, std::span<const std::byte> payload,
        QoS qos = QoS::AT_MOST_ONCE, bool retain = false);

task::Task<std::uint16_t, std::error_code>
publish(std::string topic, std::string payload,
        QoS qos = QoS::AT_MOST_ONCE, bool retain = false);
```

Publishes a message to a topic. Returns the packet ID for QoS > 0.

```cpp
// QoS 0 - fire and forget
co_await client.publish("sensors/temp", "25.5");

// QoS 1 - at least once
auto packetId = co_await client.publish("sensors/temp", "25.5", QoS::AT_LEAST_ONCE);

// QoS 2 - exactly once
co_await client.publish("commands/restart", "now", QoS::EXACTLY_ONCE);

// Retained message
co_await client.publish("status/online", "true", QoS::AT_LEAST_ONCE, true);
```

### Method `subscribe`

```cpp
task::Task<std::vector<SubscribeResult>, std::error_code>
subscribe(std::vector<Subscription> subscriptions);

task::Task<SubscribeResult, std::error_code>
subscribe(std::string topicFilter, QoS qos = QoS::AT_MOST_ONCE);
```

Subscribes to one or more topics.

```cpp
// Single topic
auto result = co_await client.subscribe("sensors/+/temperature", QoS::AT_LEAST_ONCE);

// Multiple topics
std::vector<Subscription> subs = {
    {"sensors/#", QoS::AT_LEAST_ONCE},
    {"commands/+", QoS::EXACTLY_ONCE}
};
auto results = co_await client.subscribe(subs);

for (const auto &r : *results) {
    if (r.reasonCode == ReasonCode::SUCCESS) {
        std::cout << "Subscribed with QoS " << static_cast<int>(r.grantedQos) << "\n";
    }
}
```

### Method `unsubscribe`

```cpp
task::Task<void, std::error_code>
unsubscribe(std::vector<std::string> topics);

task::Task<void, std::error_code>
unsubscribe(std::string topicFilter);
```

Unsubscribes from topics.

```cpp
co_await client.unsubscribe("sensors/temperature");
co_await client.unsubscribe({"sensors/#", "commands/+"});
```

### Method `receive`

```cpp
task::Task<Message, std::error_code> receive();
```

Receives the next published message. Blocks until a message is available or the client disconnects.

```cpp
while (client.isConnected()) {
    auto msg = co_await client.receive();
    if (!msg) {
        break;
    }

    std::cout << "Topic: " << msg->topic << "\n";
    std::cout << "Payload: " << msg->payloadString() << "\n";
    std::cout << "QoS: " << static_cast<int>(msg->qos) << "\n";
}
```

### Method `disconnect`

```cpp
task::Task<void, std::error_code>
disconnect(ReasonCode reasonCode = ReasonCode::NORMAL_DISCONNECTION);
```

Disconnects from the broker. MQTT 5.0 supports reason codes.

```cpp
co_await client.disconnect();

// MQTT 5.0 with reason
co_await client.disconnect(ReasonCode::DISCONNECT_WITH_WILL);
```

## Enum `ReasonCode`

MQTT 5.0 reason codes for various operations.

```cpp
enum class ReasonCode : std::uint8_t {
    SUCCESS = 0x00,
    NORMAL_DISCONNECTION = 0x00,
    GRANTED_QOS_0 = 0x00,
    GRANTED_QOS_1 = 0x01,
    GRANTED_QOS_2 = 0x02,
    // ... more codes
    UNSPECIFIED_ERROR = 0x80,
    MALFORMED_PACKET = 0x81,
    PROTOCOL_ERROR = 0x82,
    NOT_AUTHORIZED = 0x87,
    // ... etc
};
```

## Complete Example

```cpp
#include <asyncio/mqtt/client.h>
#include <iostream>

using namespace asyncio;
using namespace asyncio::mqtt;

task::Task<void, std::error_code> subscriber(Client &client) {
    while (client.isConnected()) {
        auto msg = co_await client.receive();
        if (!msg) break;

        std::cout << "[" << msg->topic << "] " << msg->payloadString() << "\n";
    }
    co_return {};
}

task::Task<void, std::error_code> asyncMain() {
    Config config;
    config.clientId = "asyncio-mqtt-example";
    config.keepAlive = std::chrono::seconds(30);

    // Connect
    auto client = co_await Client::connect("test.mosquitto.org", 1883, config);
    CO_EXPECT(client);

    std::cout << "Connected as " << client->clientId() << "\n";

    // Subscribe
    auto subResult = co_await client->subscribe("asyncio/test/#", QoS::AT_LEAST_ONCE);
    CO_EXPECT(subResult);
    std::cout << "Subscribed with QoS " << static_cast<int>(subResult->grantedQos) << "\n";

    // Start subscriber task
    auto subTask = subscriber(*client);

    // Publish some messages
    for (int i = 0; i < 5; ++i) {
        co_await client->publish("asyncio/test/counter", std::to_string(i), QoS::AT_LEAST_ONCE);
        co_await sleep(std::chrono::seconds(1));
    }

    // Disconnect
    co_await client->disconnect();
    std::cout << "Disconnected\n";

    co_return {};
}
```

## Error Handling

All async methods return `Task<T, std::error_code>`. Use `CO_EXPECT` for early return on error:

```cpp
auto client = co_await Client::connect("localhost", 1883, config);
CO_EXPECT(client);

auto result = co_await client->publish("topic", "message", QoS::AT_LEAST_ONCE);
if (!result) {
    if (result.error() == Client::Error::NOT_CONNECTED) {
        std::cerr << "Client disconnected\n";
    } else if (result.error() == Client::Error::TIMEOUT) {
        std::cerr << "Operation timed out\n";
    }
    co_return std::unexpected(result.error());
}
```

## Protocol Module

The `asyncio::mqtt::protocol` namespace provides low-level protocol encoding/decoding functions for advanced use cases:

```cpp
#include <asyncio/mqtt/protocol.h>

using namespace asyncio::mqtt::protocol;

// Build packets manually
auto connectPacket = buildConnectPacket(config, ProtocolVersion::V3_1_1);
auto publishPacket = buildPublishPacket("topic", payload, QoS::AT_LEAST_ONCE, false, false, packetId);

// Validate topics
bool valid = isValidTopicName("sensors/temperature");
bool validFilter = isValidTopicFilter("sensors/+/temperature");
bool matches = topicMatchesFilter("sensors/room1/temperature", "sensors/+/temperature");
```
