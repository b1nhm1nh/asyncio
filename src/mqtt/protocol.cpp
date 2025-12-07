#include <asyncio/mqtt/protocol.h>
#include <algorithm>
#include <cstring>

namespace asyncio::mqtt::protocol {

// ==================== Encoding Functions ====================

std::size_t encodeVariableByteInteger(std::uint32_t value, std::span<std::byte> buffer) {
    std::size_t i = 0;
    do {
        std::uint8_t byte = value % 128;
        value /= 128;
        if (value > 0) {
            byte |= 0x80;
        }
        buffer[i++] = static_cast<std::byte>(byte);
    } while (value > 0 && i < 4);
    return i;
}

std::size_t variableByteIntegerSize(std::uint32_t value) {
    if (value < 128) return 1;
    if (value < 16384) return 2;
    if (value < 2097152) return 3;
    return 4;
}

std::size_t encodeString(std::string_view str, std::span<std::byte> buffer) {
    encodeUint16(static_cast<std::uint16_t>(str.size()), buffer);
    std::memcpy(buffer.data() + 2, str.data(), str.size());
    return 2 + str.size();
}

std::size_t encodeBinaryData(std::span<const std::byte> data, std::span<std::byte> buffer) {
    encodeUint16(static_cast<std::uint16_t>(data.size()), buffer);
    std::memcpy(buffer.data() + 2, data.data(), data.size());
    return 2 + data.size();
}

void encodeUint16(std::uint16_t value, std::span<std::byte> buffer) {
    buffer[0] = static_cast<std::byte>((value >> 8) & 0xFF);
    buffer[1] = static_cast<std::byte>(value & 0xFF);
}

void encodeUint32(std::uint32_t value, std::span<std::byte> buffer) {
    buffer[0] = static_cast<std::byte>((value >> 24) & 0xFF);
    buffer[1] = static_cast<std::byte>((value >> 16) & 0xFF);
    buffer[2] = static_cast<std::byte>((value >> 8) & 0xFF);
    buffer[3] = static_cast<std::byte>(value & 0xFF);
}

std::size_t encodeFixedHeader(const FixedHeader &header, std::span<std::byte> buffer) {
    std::uint8_t byte1 = (static_cast<std::uint8_t>(header.type) << 4);
    if (header.dup) byte1 |= 0x08;
    byte1 |= (static_cast<std::uint8_t>(header.qos) << 1);
    if (header.retain) byte1 |= 0x01;
    buffer[0] = static_cast<std::byte>(byte1);
    return 1 + encodeVariableByteInteger(header.remainingLength, buffer.subspan(1));
}

// ==================== Decoding Functions ====================

std::expected<std::pair<std::uint32_t, std::size_t>, std::error_code>
decodeVariableByteInteger(std::span<const std::byte> buffer) {
    std::uint32_t value = 0;
    std::uint32_t multiplier = 1;
    std::size_t i = 0;

    do {
        if (i >= buffer.size() || i >= 4) {
            return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
        }
        std::uint8_t byte = static_cast<std::uint8_t>(buffer[i]);
        value += (byte & 0x7F) * multiplier;
        multiplier *= 128;
        if ((byte & 0x80) == 0) {
            return std::pair{value, i + 1};
        }
        i++;
    } while (true);
}

std::expected<std::pair<std::string, std::size_t>, std::error_code>
decodeString(std::span<const std::byte> buffer) {
    if (buffer.size() < 2) {
        return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
    }
    std::uint16_t length = decodeUint16(buffer);
    if (buffer.size() < 2 + length) {
        return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
    }
    std::string str(reinterpret_cast<const char *>(buffer.data() + 2), length);
    return std::pair{std::move(str), 2 + length};
}

std::expected<std::pair<std::vector<std::byte>, std::size_t>, std::error_code>
decodeBinaryData(std::span<const std::byte> buffer) {
    if (buffer.size() < 2) {
        return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
    }
    std::uint16_t length = decodeUint16(buffer);
    if (buffer.size() < 2 + length) {
        return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
    }
    std::vector<std::byte> data(buffer.begin() + 2, buffer.begin() + 2 + length);
    return std::pair{std::move(data), 2 + length};
}

std::uint16_t decodeUint16(std::span<const std::byte> buffer) {
    return (static_cast<std::uint16_t>(buffer[0]) << 8) |
           static_cast<std::uint16_t>(buffer[1]);
}

std::uint32_t decodeUint32(std::span<const std::byte> buffer) {
    return (static_cast<std::uint32_t>(buffer[0]) << 24) |
           (static_cast<std::uint32_t>(buffer[1]) << 16) |
           (static_cast<std::uint32_t>(buffer[2]) << 8) |
           static_cast<std::uint32_t>(buffer[3]);
}

std::expected<std::pair<FixedHeader, std::size_t>, std::error_code>
decodeFixedHeader(std::span<const std::byte> buffer) {
    if (buffer.empty()) {
        return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
    }

    FixedHeader header;
    std::uint8_t byte1 = static_cast<std::uint8_t>(buffer[0]);
    header.type = static_cast<PacketType>((byte1 >> 4) & 0x0F);
    header.dup = (byte1 & 0x08) != 0;
    header.qos = static_cast<QoS>((byte1 >> 1) & 0x03);
    header.retain = (byte1 & 0x01) != 0;

    auto lenResult = decodeVariableByteInteger(buffer.subspan(1));
    if (!lenResult) {
        return std::unexpected(lenResult.error());
    }
    header.remainingLength = lenResult->first;

    return std::pair{header, 1 + lenResult->second};
}

std::expected<std::pair<Properties, std::size_t>, std::error_code>
decodeProperties(std::span<const std::byte> buffer) {
    Properties props;

    auto lenResult = decodeVariableByteInteger(buffer);
    if (!lenResult) {
        return std::unexpected(lenResult.error());
    }

    std::size_t propsLength = lenResult->first;
    std::size_t headerSize = lenResult->second;

    if (buffer.size() < headerSize + propsLength) {
        return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
    }

    std::size_t offset = headerSize;
    std::size_t end = headerSize + propsLength;

    while (offset < end) {
        auto propType = static_cast<PropertyType>(buffer[offset++]);

        switch (propType) {
            case PropertyType::PAYLOAD_FORMAT_INDICATOR:
                props.payloadFormatIndicator = static_cast<std::uint8_t>(buffer[offset++]);
                break;

            case PropertyType::MESSAGE_EXPIRY_INTERVAL:
                props.messageExpiryInterval = decodeUint32(buffer.subspan(offset));
                offset += 4;
                break;

            case PropertyType::CONTENT_TYPE: {
                auto result = decodeString(buffer.subspan(offset));
                if (!result) return std::unexpected(result.error());
                props.contentType = std::move(result->first);
                offset += result->second;
                break;
            }

            case PropertyType::RESPONSE_TOPIC: {
                auto result = decodeString(buffer.subspan(offset));
                if (!result) return std::unexpected(result.error());
                props.responseTopic = std::move(result->first);
                offset += result->second;
                break;
            }

            case PropertyType::CORRELATION_DATA: {
                auto result = decodeBinaryData(buffer.subspan(offset));
                if (!result) return std::unexpected(result.error());
                props.correlationData = std::move(result->first);
                offset += result->second;
                break;
            }

            case PropertyType::SUBSCRIPTION_IDENTIFIER: {
                auto result = decodeVariableByteInteger(buffer.subspan(offset));
                if (!result) return std::unexpected(result.error());
                props.subscriptionIdentifier = result->first;
                offset += result->second;
                break;
            }

            case PropertyType::SESSION_EXPIRY_INTERVAL:
                props.sessionExpiryInterval = decodeUint32(buffer.subspan(offset));
                offset += 4;
                break;

            case PropertyType::ASSIGNED_CLIENT_IDENTIFIER: {
                auto result = decodeString(buffer.subspan(offset));
                if (!result) return std::unexpected(result.error());
                props.assignedClientIdentifier = std::move(result->first);
                offset += result->second;
                break;
            }

            case PropertyType::SERVER_KEEP_ALIVE:
                props.serverKeepAlive = decodeUint16(buffer.subspan(offset));
                offset += 2;
                break;

            case PropertyType::AUTHENTICATION_METHOD: {
                auto result = decodeString(buffer.subspan(offset));
                if (!result) return std::unexpected(result.error());
                props.authenticationMethod = std::move(result->first);
                offset += result->second;
                break;
            }

            case PropertyType::AUTHENTICATION_DATA: {
                auto result = decodeBinaryData(buffer.subspan(offset));
                if (!result) return std::unexpected(result.error());
                props.authenticationData = std::move(result->first);
                offset += result->second;
                break;
            }

            case PropertyType::REASON_STRING: {
                auto result = decodeString(buffer.subspan(offset));
                if (!result) return std::unexpected(result.error());
                props.reasonString = std::move(result->first);
                offset += result->second;
                break;
            }

            case PropertyType::RECEIVE_MAXIMUM:
                props.receiveMaximum = decodeUint16(buffer.subspan(offset));
                offset += 2;
                break;

            case PropertyType::TOPIC_ALIAS_MAXIMUM:
                props.topicAliasMaximum = decodeUint16(buffer.subspan(offset));
                offset += 2;
                break;

            case PropertyType::TOPIC_ALIAS:
                props.topicAlias = decodeUint16(buffer.subspan(offset));
                offset += 2;
                break;

            case PropertyType::MAXIMUM_QOS:
                props.maximumQos = static_cast<std::uint8_t>(buffer[offset++]);
                break;

            case PropertyType::RETAIN_AVAILABLE:
                props.retainAvailable = static_cast<std::uint8_t>(buffer[offset++]);
                break;

            case PropertyType::USER_PROPERTY: {
                auto keyResult = decodeString(buffer.subspan(offset));
                if (!keyResult) return std::unexpected(keyResult.error());
                offset += keyResult->second;

                auto valueResult = decodeString(buffer.subspan(offset));
                if (!valueResult) return std::unexpected(valueResult.error());
                offset += valueResult->second;

                props.userProperties.emplace_back(
                    std::move(keyResult->first),
                    std::move(valueResult->first)
                );
                break;
            }

            case PropertyType::MAXIMUM_PACKET_SIZE:
                props.maximumPacketSize = decodeUint32(buffer.subspan(offset));
                offset += 4;
                break;

            case PropertyType::WILDCARD_SUBSCRIPTION_AVAILABLE:
                props.wildcardSubscriptionAvailable = static_cast<std::uint8_t>(buffer[offset++]);
                break;

            case PropertyType::SUBSCRIPTION_IDENTIFIER_AVAILABLE:
                props.subscriptionIdentifierAvailable = static_cast<std::uint8_t>(buffer[offset++]);
                break;

            case PropertyType::SHARED_SUBSCRIPTION_AVAILABLE:
                props.sharedSubscriptionAvailable = static_cast<std::uint8_t>(buffer[offset++]);
                break;

            default:
                // Unknown property, skip
                return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
        }
    }

    return std::pair{std::move(props), end};
}

// ==================== Packet Builders ====================

std::vector<std::byte> buildConnectPacket(const Config &config, ProtocolVersion version) {
    std::vector<std::byte> packet;
    packet.reserve(256);

    // Variable header: Protocol Name
    const char *protocolName = "MQTT";
    std::size_t protocolNameLen = 4;

    // Calculate remaining length
    std::size_t remainingLength = 2 + protocolNameLen + 1 + 1 + 2;  // name + version + flags + keepalive
    remainingLength += 2 + config.clientId.size();

    std::uint8_t connectFlags = 0;
    if (config.cleanSession) connectFlags |= 0x02;

    if (config.will) {
        connectFlags |= 0x04;  // Will flag
        connectFlags |= (static_cast<std::uint8_t>(config.will->qos) << 3);
        if (config.will->retain) connectFlags |= 0x20;
        remainingLength += 2 + config.will->topic.size();
        remainingLength += 2 + config.will->payload.size();
    }

    if (!config.username.empty()) {
        connectFlags |= 0x80;
        remainingLength += 2 + config.username.size();
    }

    if (!config.password.empty()) {
        connectFlags |= 0x40;
        remainingLength += 2 + config.password.size();
    }

    // Properties (MQTT 5.0)
    std::vector<std::byte> properties;
    if (version == ProtocolVersion::V5) {
        // Add properties as needed
        remainingLength += 1;  // Empty properties (length = 0)
    }

    // Fixed header
    FixedHeader header;
    header.type = PacketType::CONNECT;
    header.remainingLength = static_cast<std::uint32_t>(remainingLength);

    packet.resize(5);  // Max fixed header size
    std::size_t headerSize = encodeFixedHeader(header, packet);
    packet.resize(headerSize + remainingLength);

    std::size_t offset = headerSize;

    // Protocol name
    encodeString(protocolName, std::span(packet).subspan(offset));
    offset += 2 + protocolNameLen;

    // Protocol version
    packet[offset++] = static_cast<std::byte>(version);

    // Connect flags
    packet[offset++] = static_cast<std::byte>(connectFlags);

    // Keep alive
    encodeUint16(static_cast<std::uint16_t>(config.keepAlive.count()), std::span(packet).subspan(offset));
    offset += 2;

    // Properties (MQTT 5.0)
    if (version == ProtocolVersion::V5) {
        packet[offset++] = static_cast<std::byte>(0);  // Empty properties
    }

    // Client ID
    offset += encodeString(config.clientId, std::span(packet).subspan(offset));

    // Will topic and message
    if (config.will) {
        offset += encodeString(config.will->topic, std::span(packet).subspan(offset));
        offset += encodeBinaryData(config.will->payload, std::span(packet).subspan(offset));
    }

    // Username
    if (!config.username.empty()) {
        offset += encodeString(config.username, std::span(packet).subspan(offset));
    }

    // Password
    if (!config.password.empty()) {
        offset += encodeString(config.password, std::span(packet).subspan(offset));
    }

    return packet;
}

std::vector<std::byte> buildPublishPacket(
    std::string_view topic,
    std::span<const std::byte> payload,
    QoS qos,
    bool retain,
    bool dup,
    std::uint16_t packetId,
    ProtocolVersion version
) {
    std::vector<std::byte> packet;

    std::size_t remainingLength = 2 + topic.size() + payload.size();
    if (qos != QoS::AT_MOST_ONCE) {
        remainingLength += 2;  // Packet ID
    }
    if (version == ProtocolVersion::V5) {
        remainingLength += 1;  // Empty properties
    }

    FixedHeader header;
    header.type = PacketType::PUBLISH;
    header.qos = qos;
    header.retain = retain;
    header.dup = dup;
    header.remainingLength = static_cast<std::uint32_t>(remainingLength);

    packet.resize(5);
    std::size_t headerSize = encodeFixedHeader(header, packet);
    packet.resize(headerSize + remainingLength);

    std::size_t offset = headerSize;

    // Topic
    offset += encodeString(topic, std::span(packet).subspan(offset));

    // Packet ID (QoS > 0)
    if (qos != QoS::AT_MOST_ONCE) {
        encodeUint16(packetId, std::span(packet).subspan(offset));
        offset += 2;
    }

    // Properties (MQTT 5.0)
    if (version == ProtocolVersion::V5) {
        packet[offset++] = static_cast<std::byte>(0);
    }

    // Payload
    std::memcpy(packet.data() + offset, payload.data(), payload.size());

    return packet;
}

std::vector<std::byte> buildPubackPacket(std::uint16_t packetId, ProtocolVersion version, ReasonCode reasonCode) {
    std::vector<std::byte> packet;

    std::size_t remainingLength = 2;  // Packet ID
    if (version == ProtocolVersion::V5) {
        remainingLength += 1;  // Reason code
        remainingLength += 1;  // Empty properties
    }

    FixedHeader header;
    header.type = PacketType::PUBACK;
    header.remainingLength = static_cast<std::uint32_t>(remainingLength);

    packet.resize(5);
    std::size_t headerSize = encodeFixedHeader(header, packet);
    packet.resize(headerSize + remainingLength);

    std::size_t offset = headerSize;
    encodeUint16(packetId, std::span(packet).subspan(offset));
    offset += 2;

    if (version == ProtocolVersion::V5) {
        packet[offset++] = static_cast<std::byte>(reasonCode);
        packet[offset++] = static_cast<std::byte>(0);  // Empty properties
    }

    return packet;
}

std::vector<std::byte> buildPubrecPacket(std::uint16_t packetId, ProtocolVersion version, ReasonCode reasonCode) {
    std::vector<std::byte> packet;

    std::size_t remainingLength = 2;
    if (version == ProtocolVersion::V5) {
        remainingLength += 2;
    }

    FixedHeader header;
    header.type = PacketType::PUBREC;
    header.remainingLength = static_cast<std::uint32_t>(remainingLength);

    packet.resize(5);
    std::size_t headerSize = encodeFixedHeader(header, packet);
    packet.resize(headerSize + remainingLength);

    std::size_t offset = headerSize;
    encodeUint16(packetId, std::span(packet).subspan(offset));
    offset += 2;

    if (version == ProtocolVersion::V5) {
        packet[offset++] = static_cast<std::byte>(reasonCode);
        packet[offset++] = static_cast<std::byte>(0);
    }

    return packet;
}

std::vector<std::byte> buildPubrelPacket(std::uint16_t packetId, ProtocolVersion version, ReasonCode reasonCode) {
    std::vector<std::byte> packet;

    std::size_t remainingLength = 2;
    if (version == ProtocolVersion::V5) {
        remainingLength += 2;
    }

    FixedHeader header;
    header.type = PacketType::PUBREL;
    header.qos = QoS::AT_LEAST_ONCE;  // PUBREL has fixed QoS 1
    header.remainingLength = static_cast<std::uint32_t>(remainingLength);

    packet.resize(5);
    std::size_t headerSize = encodeFixedHeader(header, packet);
    packet.resize(headerSize + remainingLength);

    std::size_t offset = headerSize;
    encodeUint16(packetId, std::span(packet).subspan(offset));
    offset += 2;

    if (version == ProtocolVersion::V5) {
        packet[offset++] = static_cast<std::byte>(reasonCode);
        packet[offset++] = static_cast<std::byte>(0);
    }

    return packet;
}

std::vector<std::byte> buildPubcompPacket(std::uint16_t packetId, ProtocolVersion version, ReasonCode reasonCode) {
    std::vector<std::byte> packet;

    std::size_t remainingLength = 2;
    if (version == ProtocolVersion::V5) {
        remainingLength += 2;
    }

    FixedHeader header;
    header.type = PacketType::PUBCOMP;
    header.remainingLength = static_cast<std::uint32_t>(remainingLength);

    packet.resize(5);
    std::size_t headerSize = encodeFixedHeader(header, packet);
    packet.resize(headerSize + remainingLength);

    std::size_t offset = headerSize;
    encodeUint16(packetId, std::span(packet).subspan(offset));
    offset += 2;

    if (version == ProtocolVersion::V5) {
        packet[offset++] = static_cast<std::byte>(reasonCode);
        packet[offset++] = static_cast<std::byte>(0);
    }

    return packet;
}

std::vector<std::byte> buildSubscribePacket(
    std::uint16_t packetId,
    const std::vector<Subscription> &subscriptions,
    ProtocolVersion version
) {
    std::vector<std::byte> packet;

    std::size_t remainingLength = 2;  // Packet ID
    if (version == ProtocolVersion::V5) {
        remainingLength += 1;  // Empty properties
    }
    for (const auto &sub : subscriptions) {
        remainingLength += 2 + sub.topicFilter.size() + 1;  // topic + options
    }

    FixedHeader header;
    header.type = PacketType::SUBSCRIBE;
    header.qos = QoS::AT_LEAST_ONCE;  // SUBSCRIBE has fixed QoS 1
    header.remainingLength = static_cast<std::uint32_t>(remainingLength);

    packet.resize(5);
    std::size_t headerSize = encodeFixedHeader(header, packet);
    packet.resize(headerSize + remainingLength);

    std::size_t offset = headerSize;
    encodeUint16(packetId, std::span(packet).subspan(offset));
    offset += 2;

    if (version == ProtocolVersion::V5) {
        packet[offset++] = static_cast<std::byte>(0);  // Empty properties
    }

    for (const auto &sub : subscriptions) {
        offset += encodeString(sub.topicFilter, std::span(packet).subspan(offset));

        std::uint8_t options = static_cast<std::uint8_t>(sub.options.maxQos);
        if (version == ProtocolVersion::V5) {
            if (sub.options.noLocal) options |= 0x04;
            if (sub.options.retainAsPublished) options |= 0x08;
            options |= (static_cast<std::uint8_t>(sub.options.retainHandling) << 4);
        }
        packet[offset++] = static_cast<std::byte>(options);
    }

    return packet;
}

std::vector<std::byte> buildUnsubscribePacket(
    std::uint16_t packetId,
    const std::vector<std::string> &topics,
    ProtocolVersion version
) {
    std::vector<std::byte> packet;

    std::size_t remainingLength = 2;  // Packet ID
    if (version == ProtocolVersion::V5) {
        remainingLength += 1;  // Empty properties
    }
    for (const auto &topic : topics) {
        remainingLength += 2 + topic.size();
    }

    FixedHeader header;
    header.type = PacketType::UNSUBSCRIBE;
    header.qos = QoS::AT_LEAST_ONCE;  // UNSUBSCRIBE has fixed QoS 1
    header.remainingLength = static_cast<std::uint32_t>(remainingLength);

    packet.resize(5);
    std::size_t headerSize = encodeFixedHeader(header, packet);
    packet.resize(headerSize + remainingLength);

    std::size_t offset = headerSize;
    encodeUint16(packetId, std::span(packet).subspan(offset));
    offset += 2;

    if (version == ProtocolVersion::V5) {
        packet[offset++] = static_cast<std::byte>(0);
    }

    for (const auto &topic : topics) {
        offset += encodeString(topic, std::span(packet).subspan(offset));
    }

    return packet;
}

std::vector<std::byte> buildPingreqPacket() {
    return {static_cast<std::byte>(0xC0), static_cast<std::byte>(0x00)};
}

std::vector<std::byte> buildDisconnectPacket(ProtocolVersion version, ReasonCode reasonCode) {
    if (version == ProtocolVersion::V3_1_1) {
        return {static_cast<std::byte>(0xE0), static_cast<std::byte>(0x00)};
    }

    // MQTT 5.0
    std::vector<std::byte> packet;
    std::size_t remainingLength = 1 + 1;  // Reason code + empty properties

    FixedHeader header;
    header.type = PacketType::DISCONNECT;
    header.remainingLength = static_cast<std::uint32_t>(remainingLength);

    packet.resize(5);
    std::size_t headerSize = encodeFixedHeader(header, packet);
    packet.resize(headerSize + remainingLength);

    packet[headerSize] = static_cast<std::byte>(reasonCode);
    packet[headerSize + 1] = static_cast<std::byte>(0);  // Empty properties

    return packet;
}

// ==================== Packet Parsers ====================

std::expected<ConnackPacket, std::error_code>
parseConnackPacket(std::span<const std::byte> data, ProtocolVersion version) {
    if (data.size() < 2) {
        return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
    }

    ConnackPacket packet;
    packet.sessionPresent = (static_cast<std::uint8_t>(data[0]) & 0x01) != 0;
    packet.reasonCode = static_cast<ReasonCode>(data[1]);

    if (version == ProtocolVersion::V5 && data.size() > 2) {
        auto propsResult = decodeProperties(data.subspan(2));
        if (propsResult) {
            packet.properties = std::move(propsResult->first);
        }
    }

    return packet;
}

std::expected<SubackPacket, std::error_code>
parseSubackPacket(std::span<const std::byte> data, ProtocolVersion version) {
    if (data.size() < 3) {
        return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
    }

    SubackPacket packet;
    packet.packetId = decodeUint16(data);

    std::size_t offset = 2;

    if (version == ProtocolVersion::V5) {
        auto propsResult = decodeProperties(data.subspan(offset));
        if (!propsResult) {
            return std::unexpected(propsResult.error());
        }
        packet.properties = std::move(propsResult->first);
        offset += propsResult->second;
    }

    while (offset < data.size()) {
        packet.reasonCodes.push_back(static_cast<ReasonCode>(data[offset++]));
    }

    return packet;
}

std::expected<UnsubackPacket, std::error_code>
parseUnsubackPacket(std::span<const std::byte> data, ProtocolVersion version) {
    if (data.size() < 2) {
        return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
    }

    UnsubackPacket packet;
    packet.packetId = decodeUint16(data);

    if (version == ProtocolVersion::V5 && data.size() > 2) {
        std::size_t offset = 2;
        auto propsResult = decodeProperties(data.subspan(offset));
        if (!propsResult) {
            return std::unexpected(propsResult.error());
        }
        packet.properties = std::move(propsResult->first);
        offset += propsResult->second;

        while (offset < data.size()) {
            packet.reasonCodes.push_back(static_cast<ReasonCode>(data[offset++]));
        }
    }

    return packet;
}

std::expected<PublishPacket, std::error_code>
parsePublishPacket(const FixedHeader &header, std::span<const std::byte> data, ProtocolVersion version) {
    PublishPacket packet;
    packet.qos = header.qos;
    packet.retain = header.retain;
    packet.dup = header.dup;

    auto topicResult = decodeString(data);
    if (!topicResult) {
        return std::unexpected(topicResult.error());
    }
    packet.topic = std::move(topicResult->first);
    std::size_t offset = topicResult->second;

    if (header.qos != QoS::AT_MOST_ONCE) {
        if (data.size() < offset + 2) {
            return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
        }
        packet.packetId = decodeUint16(data.subspan(offset));
        offset += 2;
    }

    if (version == ProtocolVersion::V5) {
        auto propsResult = decodeProperties(data.subspan(offset));
        if (!propsResult) {
            return std::unexpected(propsResult.error());
        }
        packet.properties = std::move(propsResult->first);
        offset += propsResult->second;
    }

    packet.payload.assign(data.begin() + offset, data.end());

    return packet;
}

std::expected<AckPacket, std::error_code>
parseAckPacket(std::span<const std::byte> data, ProtocolVersion version) {
    if (data.size() < 2) {
        return std::unexpected(make_error_code(Client::Error::PROTOCOL_ERROR));
    }

    AckPacket packet;
    packet.packetId = decodeUint16(data);

    if (version == ProtocolVersion::V5 && data.size() > 2) {
        packet.reasonCode = static_cast<ReasonCode>(data[2]);
        if (data.size() > 3) {
            auto propsResult = decodeProperties(data.subspan(3));
            if (propsResult) {
                packet.properties = std::move(propsResult->first);
            }
        }
    }

    return packet;
}

// ==================== Validation ====================

bool isValidTopicName(std::string_view topic) {
    if (topic.empty() || topic.size() > 65535) {
        return false;
    }

    // Topic name cannot contain wildcards
    return topic.find('#') == std::string_view::npos &&
           topic.find('+') == std::string_view::npos;
}

bool isValidTopicFilter(std::string_view filter) {
    if (filter.empty() || filter.size() > 65535) {
        return false;
    }

    // Check wildcard rules
    for (std::size_t i = 0; i < filter.size(); ++i) {
        if (filter[i] == '+') {
            // '+' must occupy entire level
            if ((i > 0 && filter[i - 1] != '/') ||
                (i + 1 < filter.size() && filter[i + 1] != '/')) {
                return false;
            }
        } else if (filter[i] == '#') {
            // '#' must be last character and preceded by '/' (or be the entire filter)
            if (i + 1 != filter.size() || (i > 0 && filter[i - 1] != '/')) {
                return false;
            }
        }
    }

    return true;
}

bool topicMatchesFilter(std::string_view topic, std::string_view filter) {
    std::size_t ti = 0, fi = 0;

    while (fi < filter.size()) {
        if (filter[fi] == '#') {
            return true;  // '#' matches everything remaining (including nothing)
        }

        if (filter[fi] == '+') {
            // Skip to next '/' in topic (or end of topic)
            while (ti < topic.size() && topic[ti] != '/') {
                ++ti;
            }
            ++fi;
        } else {
            if (ti >= topic.size()) {
                // Topic exhausted - only valid if remaining filter is "/#"
                return (fi + 2 == filter.size() &&
                        filter[fi] == '/' && filter[fi + 1] == '#');
            }
            if (topic[ti] != filter[fi]) {
                return false;
            }
            ++ti;
            ++fi;
        }
    }

    return ti == topic.size();
}

} // namespace asyncio::mqtt::protocol
