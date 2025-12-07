#ifndef ASYNCIO_MQTT_PROTOCOL_H
#define ASYNCIO_MQTT_PROTOCOL_H

#include <asyncio/mqtt/client.h>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace asyncio::mqtt::protocol {
    /// Maximum remaining length value (268,435,455 bytes)
    constexpr std::uint32_t MAX_REMAINING_LENGTH = 0x0FFFFFFF;

    /// Fixed header structure
    struct FixedHeader {
        PacketType type;
        bool dup = false;
        QoS qos = QoS::AT_MOST_ONCE;
        bool retain = false;
        std::uint32_t remainingLength = 0;
    };

    /// MQTT 5.0 Property Types
    enum class PropertyType : std::uint8_t {
        PAYLOAD_FORMAT_INDICATOR = 0x01,
        MESSAGE_EXPIRY_INTERVAL = 0x02,
        CONTENT_TYPE = 0x03,
        RESPONSE_TOPIC = 0x08,
        CORRELATION_DATA = 0x09,
        SUBSCRIPTION_IDENTIFIER = 0x0B,
        SESSION_EXPIRY_INTERVAL = 0x11,
        ASSIGNED_CLIENT_IDENTIFIER = 0x12,
        SERVER_KEEP_ALIVE = 0x13,
        AUTHENTICATION_METHOD = 0x15,
        AUTHENTICATION_DATA = 0x16,
        REQUEST_PROBLEM_INFORMATION = 0x17,
        WILL_DELAY_INTERVAL = 0x18,
        REQUEST_RESPONSE_INFORMATION = 0x19,
        RESPONSE_INFORMATION = 0x1A,
        SERVER_REFERENCE = 0x1C,
        REASON_STRING = 0x1F,
        RECEIVE_MAXIMUM = 0x21,
        TOPIC_ALIAS_MAXIMUM = 0x22,
        TOPIC_ALIAS = 0x23,
        MAXIMUM_QOS = 0x24,
        RETAIN_AVAILABLE = 0x25,
        USER_PROPERTY = 0x26,
        MAXIMUM_PACKET_SIZE = 0x27,
        WILDCARD_SUBSCRIPTION_AVAILABLE = 0x28,
        SUBSCRIPTION_IDENTIFIER_AVAILABLE = 0x29,
        SHARED_SUBSCRIPTION_AVAILABLE = 0x2A
    };

    /// MQTT 5.0 Properties container
    struct Properties {
        std::optional<std::uint8_t> payloadFormatIndicator;
        std::optional<std::uint32_t> messageExpiryInterval;
        std::optional<std::string> contentType;
        std::optional<std::string> responseTopic;
        std::optional<std::vector<std::byte>> correlationData;
        std::optional<std::uint32_t> subscriptionIdentifier;
        std::optional<std::uint32_t> sessionExpiryInterval;
        std::optional<std::string> assignedClientIdentifier;
        std::optional<std::uint16_t> serverKeepAlive;
        std::optional<std::string> authenticationMethod;
        std::optional<std::vector<std::byte>> authenticationData;
        std::optional<std::uint8_t> requestProblemInformation;
        std::optional<std::uint32_t> willDelayInterval;
        std::optional<std::uint8_t> requestResponseInformation;
        std::optional<std::string> responseInformation;
        std::optional<std::string> serverReference;
        std::optional<std::string> reasonString;
        std::optional<std::uint16_t> receiveMaximum;
        std::optional<std::uint16_t> topicAliasMaximum;
        std::optional<std::uint16_t> topicAlias;
        std::optional<std::uint8_t> maximumQos;
        std::optional<std::uint8_t> retainAvailable;
        std::vector<std::pair<std::string, std::string>> userProperties;
        std::optional<std::uint32_t> maximumPacketSize;
        std::optional<std::uint8_t> wildcardSubscriptionAvailable;
        std::optional<std::uint8_t> subscriptionIdentifierAvailable;
        std::optional<std::uint8_t> sharedSubscriptionAvailable;
    };

    // ==================== Encoding Functions ====================

    /// Encode variable byte integer (1-4 bytes)
    /// Returns number of bytes written
    std::size_t encodeVariableByteInteger(std::uint32_t value, std::span<std::byte> buffer);

    /// Get encoded size of variable byte integer
    std::size_t variableByteIntegerSize(std::uint32_t value);

    /// Encode UTF-8 string with length prefix
    std::size_t encodeString(std::string_view str, std::span<std::byte> buffer);

    /// Encode binary data with length prefix
    std::size_t encodeBinaryData(std::span<const std::byte> data, std::span<std::byte> buffer);

    /// Encode 16-bit integer (big-endian)
    void encodeUint16(std::uint16_t value, std::span<std::byte> buffer);

    /// Encode 32-bit integer (big-endian)
    void encodeUint32(std::uint32_t value, std::span<std::byte> buffer);

    /// Encode fixed header
    std::size_t encodeFixedHeader(const FixedHeader &header, std::span<std::byte> buffer);

    // ==================== Decoding Functions ====================

    /// Decode variable byte integer
    /// Returns {value, bytes_consumed} or error
    std::expected<std::pair<std::uint32_t, std::size_t>, std::error_code>
    decodeVariableByteInteger(std::span<const std::byte> buffer);

    /// Decode UTF-8 string with length prefix
    /// Returns {string, bytes_consumed} or error
    std::expected<std::pair<std::string, std::size_t>, std::error_code>
    decodeString(std::span<const std::byte> buffer);

    /// Decode binary data with length prefix
    /// Returns {data, bytes_consumed} or error
    std::expected<std::pair<std::vector<std::byte>, std::size_t>, std::error_code>
    decodeBinaryData(std::span<const std::byte> buffer);

    /// Decode 16-bit integer (big-endian)
    std::uint16_t decodeUint16(std::span<const std::byte> buffer);

    /// Decode 32-bit integer (big-endian)
    std::uint32_t decodeUint32(std::span<const std::byte> buffer);

    /// Decode fixed header
    /// Returns {header, bytes_consumed} or error
    std::expected<std::pair<FixedHeader, std::size_t>, std::error_code>
    decodeFixedHeader(std::span<const std::byte> buffer);

    /// Decode MQTT 5.0 properties
    std::expected<std::pair<Properties, std::size_t>, std::error_code>
    decodeProperties(std::span<const std::byte> buffer);

    // ==================== Packet Builders ====================

    /// Build CONNECT packet
    std::vector<std::byte> buildConnectPacket(
        const Config &config,
        ProtocolVersion version = ProtocolVersion::V3_1_1
    );

    /// Build PUBLISH packet
    std::vector<std::byte> buildPublishPacket(
        std::string_view topic,
        std::span<const std::byte> payload,
        QoS qos,
        bool retain,
        bool dup,
        std::uint16_t packetId,
        ProtocolVersion version = ProtocolVersion::V3_1_1
    );

    /// Build PUBACK packet
    std::vector<std::byte> buildPubackPacket(
        std::uint16_t packetId,
        ProtocolVersion version = ProtocolVersion::V3_1_1,
        ReasonCode reasonCode = ReasonCode::SUCCESS
    );

    /// Build PUBREC packet
    std::vector<std::byte> buildPubrecPacket(
        std::uint16_t packetId,
        ProtocolVersion version = ProtocolVersion::V3_1_1,
        ReasonCode reasonCode = ReasonCode::SUCCESS
    );

    /// Build PUBREL packet
    std::vector<std::byte> buildPubrelPacket(
        std::uint16_t packetId,
        ProtocolVersion version = ProtocolVersion::V3_1_1,
        ReasonCode reasonCode = ReasonCode::SUCCESS
    );

    /// Build PUBCOMP packet
    std::vector<std::byte> buildPubcompPacket(
        std::uint16_t packetId,
        ProtocolVersion version = ProtocolVersion::V3_1_1,
        ReasonCode reasonCode = ReasonCode::SUCCESS
    );

    /// Build SUBSCRIBE packet
    std::vector<std::byte> buildSubscribePacket(
        std::uint16_t packetId,
        const std::vector<Subscription> &subscriptions,
        ProtocolVersion version = ProtocolVersion::V3_1_1
    );

    /// Build UNSUBSCRIBE packet
    std::vector<std::byte> buildUnsubscribePacket(
        std::uint16_t packetId,
        const std::vector<std::string> &topics,
        ProtocolVersion version = ProtocolVersion::V3_1_1
    );

    /// Build PINGREQ packet
    std::vector<std::byte> buildPingreqPacket();

    /// Build DISCONNECT packet
    std::vector<std::byte> buildDisconnectPacket(
        ProtocolVersion version = ProtocolVersion::V3_1_1,
        ReasonCode reasonCode = ReasonCode::NORMAL_DISCONNECTION
    );

    // ==================== Packet Parsers ====================

    /// CONNACK packet data
    struct ConnackPacket {
        bool sessionPresent = false;
        ReasonCode reasonCode = ReasonCode::SUCCESS;
        Properties properties;  // MQTT 5.0 only
    };

    /// Parse CONNACK packet
    std::expected<ConnackPacket, std::error_code>
    parseConnackPacket(std::span<const std::byte> data, ProtocolVersion version);

    /// SUBACK packet data
    struct SubackPacket {
        std::uint16_t packetId = 0;
        std::vector<ReasonCode> reasonCodes;
        Properties properties;  // MQTT 5.0 only
    };

    /// Parse SUBACK packet
    std::expected<SubackPacket, std::error_code>
    parseSubackPacket(std::span<const std::byte> data, ProtocolVersion version);

    /// UNSUBACK packet data
    struct UnsubackPacket {
        std::uint16_t packetId = 0;
        std::vector<ReasonCode> reasonCodes;  // MQTT 5.0 only
        Properties properties;  // MQTT 5.0 only
    };

    /// Parse UNSUBACK packet
    std::expected<UnsubackPacket, std::error_code>
    parseUnsubackPacket(std::span<const std::byte> data, ProtocolVersion version);

    /// PUBLISH packet data
    struct PublishPacket {
        std::string topic;
        std::vector<std::byte> payload;
        QoS qos = QoS::AT_MOST_ONCE;
        bool retain = false;
        bool dup = false;
        std::uint16_t packetId = 0;
        Properties properties;  // MQTT 5.0 only
    };

    /// Parse PUBLISH packet
    std::expected<PublishPacket, std::error_code>
    parsePublishPacket(const FixedHeader &header, std::span<const std::byte> data, ProtocolVersion version);

    /// Simple acknowledgment packet (PUBACK, PUBREC, PUBREL, PUBCOMP)
    struct AckPacket {
        std::uint16_t packetId = 0;
        ReasonCode reasonCode = ReasonCode::SUCCESS;  // MQTT 5.0 only
        Properties properties;  // MQTT 5.0 only
    };

    /// Parse acknowledgment packet
    std::expected<AckPacket, std::error_code>
    parseAckPacket(std::span<const std::byte> data, ProtocolVersion version);

    // ==================== Validation ====================

    /// Validate topic name (for publishing)
    bool isValidTopicName(std::string_view topic);

    /// Validate topic filter (for subscribing)
    bool isValidTopicFilter(std::string_view filter);

    /// Check if topic matches filter
    bool topicMatchesFilter(std::string_view topic, std::string_view filter);
}

#endif // ASYNCIO_MQTT_PROTOCOL_H
