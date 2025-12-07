#ifndef ASYNCIO_MQTT_CLIENT_H
#define ASYNCIO_MQTT_CLIENT_H

#include <asyncio/io.h>
#include <asyncio/net/stream.h>
#include <asyncio/net/tls.h>
#include <asyncio/channel.h>
#include <asyncio/sync/mutex.h>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace asyncio::mqtt {
    constexpr std::uint16_t DEFAULT_PORT = 1883;
    constexpr std::uint16_t DEFAULT_TLS_PORT = 8883;

    /// MQTT Protocol Version
    enum class ProtocolVersion : std::uint8_t {
        V3_1_1 = 4,  // MQTT 3.1.1
        V5 = 5       // MQTT 5.0
    };

    /// Quality of Service levels
    enum class QoS : std::uint8_t {
        AT_MOST_ONCE = 0,   // Fire and forget (QoS 0)
        AT_LEAST_ONCE = 1,  // Acknowledged delivery (QoS 1)
        EXACTLY_ONCE = 2    // Assured delivery (QoS 2)
    };

    /// MQTT Packet Types
    enum class PacketType : std::uint8_t {
        CONNECT = 1,
        CONNACK = 2,
        PUBLISH = 3,
        PUBACK = 4,
        PUBREC = 5,
        PUBREL = 6,
        PUBCOMP = 7,
        SUBSCRIBE = 8,
        SUBACK = 9,
        UNSUBSCRIBE = 10,
        UNSUBACK = 11,
        PINGREQ = 12,
        PINGRESP = 13,
        DISCONNECT = 14,
        AUTH = 15  // MQTT 5.0 only
    };

    /// MQTT 5.0 Reason Codes
    enum class ReasonCode : std::uint8_t {
        SUCCESS = 0x00,
        NORMAL_DISCONNECTION = 0x00,
        GRANTED_QOS_0 = 0x00,
        GRANTED_QOS_1 = 0x01,
        GRANTED_QOS_2 = 0x02,
        DISCONNECT_WITH_WILL = 0x04,
        NO_MATCHING_SUBSCRIBERS = 0x10,
        NO_SUBSCRIPTION_EXISTED = 0x11,
        CONTINUE_AUTHENTICATION = 0x18,
        RE_AUTHENTICATE = 0x19,
        UNSPECIFIED_ERROR = 0x80,
        MALFORMED_PACKET = 0x81,
        PROTOCOL_ERROR = 0x82,
        IMPLEMENTATION_SPECIFIC_ERROR = 0x83,
        UNSUPPORTED_PROTOCOL_VERSION = 0x84,
        CLIENT_IDENTIFIER_NOT_VALID = 0x85,
        BAD_USERNAME_OR_PASSWORD = 0x86,
        NOT_AUTHORIZED = 0x87,
        SERVER_UNAVAILABLE = 0x88,
        SERVER_BUSY = 0x89,
        BANNED = 0x8A,
        SERVER_SHUTTING_DOWN = 0x8B,
        BAD_AUTHENTICATION_METHOD = 0x8C,
        KEEP_ALIVE_TIMEOUT = 0x8D,
        SESSION_TAKEN_OVER = 0x8E,
        TOPIC_FILTER_INVALID = 0x8F,
        TOPIC_NAME_INVALID = 0x90,
        PACKET_IDENTIFIER_IN_USE = 0x91,
        PACKET_IDENTIFIER_NOT_FOUND = 0x92,
        RECEIVE_MAXIMUM_EXCEEDED = 0x93,
        TOPIC_ALIAS_INVALID = 0x94,
        PACKET_TOO_LARGE = 0x95,
        MESSAGE_RATE_TOO_HIGH = 0x96,
        QUOTA_EXCEEDED = 0x97,
        ADMINISTRATIVE_ACTION = 0x98,
        PAYLOAD_FORMAT_INVALID = 0x99,
        RETAIN_NOT_SUPPORTED = 0x9A,
        QOS_NOT_SUPPORTED = 0x9B,
        USE_ANOTHER_SERVER = 0x9C,
        SERVER_MOVED = 0x9D,
        SHARED_SUBSCRIPTIONS_NOT_SUPPORTED = 0x9E,
        CONNECTION_RATE_EXCEEDED = 0x9F,
        MAXIMUM_CONNECT_TIME = 0xA0,
        SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED = 0xA1,
        WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED = 0xA2
    };

    class ReasonCodeCategory final : public std::error_category {
    public:
        static const std::error_category &instance();

        [[nodiscard]] const char *name() const noexcept override {
            return "asyncio::mqtt::ReasonCode";
        }

        [[nodiscard]] std::string message(int value) const override;
    };

    inline std::error_code make_error_code(ReasonCode e) {
        return {std::to_underlying(e), errorCategoryInstance<ReasonCodeCategory>()};
    }

    /// Will message sent by broker when client disconnects unexpectedly
    struct Will {
        std::string topic;
        std::vector<std::byte> payload;
        QoS qos = QoS::AT_MOST_ONCE;
        bool retain = false;
    };

    /// Subscription options (MQTT 5.0)
    struct SubscribeOptions {
        QoS maxQos = QoS::EXACTLY_ONCE;
        bool noLocal = false;
        bool retainAsPublished = false;
        enum class RetainHandling : std::uint8_t {
            SEND_ON_SUBSCRIBE = 0,
            SEND_IF_NEW_SUBSCRIPTION = 1,
            DO_NOT_SEND = 2
        } retainHandling = RetainHandling::SEND_ON_SUBSCRIBE;
    };

    /// Topic subscription with QoS
    struct Subscription {
        std::string topicFilter;
        SubscribeOptions options;

        Subscription(std::string filter, QoS qos = QoS::AT_MOST_ONCE)
            : topicFilter(std::move(filter)), options{qos} {}

        Subscription(std::string filter, SubscribeOptions opts)
            : topicFilter(std::move(filter)), options(opts) {}
    };

    /// Received message
    struct Message {
        std::string topic;
        std::vector<std::byte> payload;
        QoS qos = QoS::AT_MOST_ONCE;
        bool retain = false;
        bool dup = false;
        std::uint16_t packetId = 0;

        /// Get payload as string
        [[nodiscard]] std::string payloadString() const {
            return {reinterpret_cast<const char *>(payload.data()), payload.size()};
        }
    };

    /// Subscribe result for a single topic
    struct SubscribeResult {
        ReasonCode reasonCode;
        QoS grantedQos;
    };

    /// Client configuration
    struct Config {
        std::string clientId;
        std::string username;
        std::string password;
        std::optional<Will> will;
        bool cleanSession = true;  // MQTT 3.1.1: clean session, MQTT 5.0: clean start
        std::chrono::seconds keepAlive{60};
        ProtocolVersion protocolVersion = ProtocolVersion::V3_1_1;

        /// Connection timeout
        std::chrono::milliseconds connectTimeout{30000};

        /// Maximum reconnect delay for exponential backoff
        std::chrono::seconds maxReconnectDelay{60};

        /// Initial reconnect delay
        std::chrono::seconds initialReconnectDelay{1};

        /// Maximum receive buffer size (0 = unlimited)
        std::size_t maxReceiveBufferSize = 256 * 1024;

        /// Maximum in-flight QoS 1/2 messages
        std::uint16_t receiveMaximum = 65535;
    };

    /// TLS configuration for secure connections
    struct TLSConfig {
        std::string certFile;
        std::string keyFile;
        std::string caFile;
        bool verifyPeer = true;
    };

    /// MQTT Client
    class Client {
        enum class State {
            DISCONNECTED,
            CONNECTING,
            CONNECTED,
            DISCONNECTING
        };

    public:
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

        Client(Config config);
        ~Client();

        Client(const Client &) = delete;
        Client &operator=(const Client &) = delete;
        Client(Client &&) noexcept;
        Client &operator=(Client &&) noexcept;

        /// Connect to MQTT broker
        /// @param host Broker hostname or IP
        /// @param port Broker port (default 1883)
        static task::Task<Client, std::error_code>
        connect(std::string host, std::uint16_t port, Config config);

        /// Connect to MQTT broker with TLS
        /// @param host Broker hostname or IP
        /// @param port Broker port (default 8883)
        static task::Task<Client, std::error_code>
        connect(std::string host, std::uint16_t port, Config config, TLSConfig tlsConfig);

        /// Connect using URL (mqtt://host:port or mqtts://host:port)
        static task::Task<Client, std::error_code>
        connect(std::string url, Config config);

        /// Check if client is connected
        [[nodiscard]] bool isConnected() const;

        /// Get client ID
        [[nodiscard]] const std::string &clientId() const;

        /// Publish a message
        /// @param topic Topic to publish to
        /// @param payload Message payload
        /// @param qos Quality of service level
        /// @param retain Whether broker should retain the message
        /// @return Packet ID for QoS > 0, 0 for QoS 0
        task::Task<std::uint16_t, std::error_code>
        publish(std::string topic, std::span<const std::byte> payload,
                QoS qos = QoS::AT_MOST_ONCE, bool retain = false);

        /// Publish a string message
        task::Task<std::uint16_t, std::error_code>
        publish(std::string topic, std::string payload,
                QoS qos = QoS::AT_MOST_ONCE, bool retain = false);

        /// Subscribe to topics
        /// @param subscriptions List of topic subscriptions
        /// @return Subscribe results for each topic
        task::Task<std::vector<SubscribeResult>, std::error_code>
        subscribe(std::vector<Subscription> subscriptions);

        /// Subscribe to a single topic
        task::Task<SubscribeResult, std::error_code>
        subscribe(std::string topicFilter, QoS qos = QoS::AT_MOST_ONCE);

        /// Unsubscribe from topics
        /// @param topics List of topic filters to unsubscribe from
        task::Task<void, std::error_code>
        unsubscribe(std::vector<std::string> topics);

        /// Unsubscribe from a single topic
        task::Task<void, std::error_code>
        unsubscribe(std::string topicFilter);

        /// Receive next message
        /// Blocks until a message is received or client disconnects
        task::Task<Message, std::error_code> receive();

        /// Disconnect from broker
        /// @param reasonCode Disconnect reason (MQTT 5.0 only)
        task::Task<void, std::error_code>
        disconnect(ReasonCode reasonCode = ReasonCode::NORMAL_DISCONNECTION);

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };

    /// URL parsing helper
    struct URL {
        std::string scheme;  // "mqtt" or "mqtts"
        std::string host;
        std::uint16_t port = DEFAULT_PORT;
        std::string path;

        static std::expected<URL, std::error_code> parse(std::string_view url);
    };
}

DECLARE_ERROR_CODES(asyncio::mqtt::ReasonCode, asyncio::mqtt::Client::Error)

#endif // ASYNCIO_MQTT_CLIENT_H
