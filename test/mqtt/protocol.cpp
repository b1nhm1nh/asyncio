#include <catch_extensions.h>
#include <asyncio/mqtt/protocol.h>

using namespace asyncio::mqtt;
using namespace asyncio::mqtt::protocol;

TEST_CASE("Variable byte integer encoding", "[mqtt::protocol]") {
    std::array<std::byte, 4> buffer{};

    SECTION("single byte values") {
        REQUIRE(encodeVariableByteInteger(0, buffer) == 1);
        REQUIRE(buffer[0] == std::byte{0x00});

        REQUIRE(encodeVariableByteInteger(127, buffer) == 1);
        REQUIRE(buffer[0] == std::byte{0x7F});
    }

    SECTION("two byte values") {
        REQUIRE(encodeVariableByteInteger(128, buffer) == 2);
        REQUIRE(buffer[0] == std::byte{0x80});
        REQUIRE(buffer[1] == std::byte{0x01});

        REQUIRE(encodeVariableByteInteger(16383, buffer) == 2);
        REQUIRE(buffer[0] == std::byte{0xFF});
        REQUIRE(buffer[1] == std::byte{0x7F});
    }

    SECTION("three byte values") {
        REQUIRE(encodeVariableByteInteger(16384, buffer) == 3);
        REQUIRE(buffer[0] == std::byte{0x80});
        REQUIRE(buffer[1] == std::byte{0x80});
        REQUIRE(buffer[2] == std::byte{0x01});

        REQUIRE(encodeVariableByteInteger(2097151, buffer) == 3);
        REQUIRE(buffer[0] == std::byte{0xFF});
        REQUIRE(buffer[1] == std::byte{0xFF});
        REQUIRE(buffer[2] == std::byte{0x7F});
    }

    SECTION("four byte values") {
        REQUIRE(encodeVariableByteInteger(2097152, buffer) == 4);
        REQUIRE(buffer[0] == std::byte{0x80});
        REQUIRE(buffer[1] == std::byte{0x80});
        REQUIRE(buffer[2] == std::byte{0x80});
        REQUIRE(buffer[3] == std::byte{0x01});

        REQUIRE(encodeVariableByteInteger(268435455, buffer) == 4);
        REQUIRE(buffer[0] == std::byte{0xFF});
        REQUIRE(buffer[1] == std::byte{0xFF});
        REQUIRE(buffer[2] == std::byte{0xFF});
        REQUIRE(buffer[3] == std::byte{0x7F});
    }
}

TEST_CASE("Variable byte integer decoding", "[mqtt::protocol]") {
    SECTION("single byte values") {
        std::array<std::byte, 1> buf1{std::byte{0x00}};
        auto result1 = decodeVariableByteInteger(buf1);
        REQUIRE(result1);
        REQUIRE(result1->first == 0);
        REQUIRE(result1->second == 1);

        std::array<std::byte, 1> buf2{std::byte{0x7F}};
        auto result2 = decodeVariableByteInteger(buf2);
        REQUIRE(result2);
        REQUIRE(result2->first == 127);
        REQUIRE(result2->second == 1);
    }

    SECTION("two byte values") {
        std::array<std::byte, 2> buf{std::byte{0x80}, std::byte{0x01}};
        auto result = decodeVariableByteInteger(buf);
        REQUIRE(result);
        REQUIRE(result->first == 128);
        REQUIRE(result->second == 2);
    }

    SECTION("round trip") {
        std::array<std::byte, 4> buffer{};
        std::vector<std::uint32_t> values = {0, 1, 127, 128, 16383, 16384, 2097151, 2097152, 268435455};

        for (auto value : values) {
            auto encoded = encodeVariableByteInteger(value, buffer);
            auto decoded = decodeVariableByteInteger(std::span(buffer).subspan(0, encoded));
            REQUIRE(decoded);
            REQUIRE(decoded->first == value);
        }
    }
}

TEST_CASE("Variable byte integer size", "[mqtt::protocol]") {
    REQUIRE(variableByteIntegerSize(0) == 1);
    REQUIRE(variableByteIntegerSize(127) == 1);
    REQUIRE(variableByteIntegerSize(128) == 2);
    REQUIRE(variableByteIntegerSize(16383) == 2);
    REQUIRE(variableByteIntegerSize(16384) == 3);
    REQUIRE(variableByteIntegerSize(2097151) == 3);
    REQUIRE(variableByteIntegerSize(2097152) == 4);
    REQUIRE(variableByteIntegerSize(268435455) == 4);
}

TEST_CASE("String encoding/decoding", "[mqtt::protocol]") {
    SECTION("empty string") {
        std::array<std::byte, 10> buffer{};
        auto size = encodeString("", buffer);
        REQUIRE(size == 2);
        REQUIRE(buffer[0] == std::byte{0x00});
        REQUIRE(buffer[1] == std::byte{0x00});

        auto decoded = decodeString(std::span(buffer).subspan(0, size));
        REQUIRE(decoded);
        REQUIRE(decoded->first.empty());
        REQUIRE(decoded->second == 2);
    }

    SECTION("simple string") {
        std::array<std::byte, 20> buffer{};
        auto size = encodeString("hello", buffer);
        REQUIRE(size == 7);  // 2 bytes length + 5 bytes "hello"
        REQUIRE(buffer[0] == std::byte{0x00});
        REQUIRE(buffer[1] == std::byte{0x05});

        auto decoded = decodeString(std::span(buffer).subspan(0, size));
        REQUIRE(decoded);
        REQUIRE(decoded->first == "hello");
        REQUIRE(decoded->second == 7);
    }

    SECTION("round trip") {
        std::array<std::byte, 100> buffer{};
        std::vector<std::string> strings = {"", "a", "hello", "test/topic/name", "sensors/+/temperature"};

        for (const auto& str : strings) {
            auto size = encodeString(str, buffer);
            auto decoded = decodeString(std::span(buffer).subspan(0, size));
            REQUIRE(decoded);
            REQUIRE(decoded->first == str);
        }
    }
}

TEST_CASE("Uint16 encoding/decoding", "[mqtt::protocol]") {
    std::array<std::byte, 2> buffer{};

    SECTION("zero") {
        encodeUint16(0, buffer);
        REQUIRE(buffer[0] == std::byte{0x00});
        REQUIRE(buffer[1] == std::byte{0x00});
        REQUIRE(decodeUint16(buffer) == 0);
    }

    SECTION("max value") {
        encodeUint16(65535, buffer);
        REQUIRE(buffer[0] == std::byte{0xFF});
        REQUIRE(buffer[1] == std::byte{0xFF});
        REQUIRE(decodeUint16(buffer) == 65535);
    }

    SECTION("typical value") {
        encodeUint16(1883, buffer);
        REQUIRE(decodeUint16(buffer) == 1883);
    }
}

TEST_CASE("Fixed header encoding/decoding", "[mqtt::protocol]") {
    SECTION("CONNECT packet") {
        FixedHeader header;
        header.type = PacketType::CONNECT;
        header.remainingLength = 10;

        std::array<std::byte, 5> buffer{};
        auto size = encodeFixedHeader(header, buffer);
        REQUIRE(size == 2);
        REQUIRE(buffer[0] == std::byte{0x10});  // CONNECT = 1, flags = 0
        REQUIRE(buffer[1] == std::byte{0x0A});  // remaining length = 10

        auto decoded = decodeFixedHeader(std::span(buffer).subspan(0, size));
        REQUIRE(decoded);
        REQUIRE(decoded->first.type == PacketType::CONNECT);
        REQUIRE(decoded->first.remainingLength == 10);
    }

    SECTION("PUBLISH with QoS 1") {
        FixedHeader header;
        header.type = PacketType::PUBLISH;
        header.qos = QoS::AT_LEAST_ONCE;
        header.retain = true;
        header.remainingLength = 50;

        std::array<std::byte, 5> buffer{};
        auto size = encodeFixedHeader(header, buffer);
        REQUIRE(size == 2);
        REQUIRE(buffer[0] == std::byte{0x33});  // PUBLISH = 3, QoS=1, retain=1

        auto decoded = decodeFixedHeader(std::span(buffer).subspan(0, size));
        REQUIRE(decoded);
        REQUIRE(decoded->first.type == PacketType::PUBLISH);
        REQUIRE(decoded->first.qos == QoS::AT_LEAST_ONCE);
        REQUIRE(decoded->first.retain == true);
        REQUIRE(decoded->first.remainingLength == 50);
    }

    SECTION("SUBSCRIBE packet") {
        FixedHeader header;
        header.type = PacketType::SUBSCRIBE;
        header.qos = QoS::AT_LEAST_ONCE;  // Fixed QoS for SUBSCRIBE
        header.remainingLength = 100;

        std::array<std::byte, 5> buffer{};
        auto size = encodeFixedHeader(header, buffer);

        auto decoded = decodeFixedHeader(std::span(buffer).subspan(0, size));
        REQUIRE(decoded);
        REQUIRE(decoded->first.type == PacketType::SUBSCRIBE);
        REQUIRE(decoded->first.qos == QoS::AT_LEAST_ONCE);
    }
}

TEST_CASE("Topic name validation", "[mqtt::protocol]") {
    SECTION("valid topic names") {
        REQUIRE(isValidTopicName("test"));
        REQUIRE(isValidTopicName("test/topic"));
        REQUIRE(isValidTopicName("sensors/temperature"));
        REQUIRE(isValidTopicName("a/b/c/d/e"));
        REQUIRE(isValidTopicName("/leading/slash"));
        REQUIRE(isValidTopicName("trailing/slash/"));
        REQUIRE(isValidTopicName("$SYS/broker/uptime"));
    }

    SECTION("invalid topic names") {
        REQUIRE_FALSE(isValidTopicName(""));  // Empty
        REQUIRE_FALSE(isValidTopicName("test/+/topic"));  // Contains +
        REQUIRE_FALSE(isValidTopicName("test/#"));  // Contains #
        REQUIRE_FALSE(isValidTopicName("test/+"));  // Contains +
        REQUIRE_FALSE(isValidTopicName("#"));  // Only #
        REQUIRE_FALSE(isValidTopicName("+"));  // Only +
    }
}

TEST_CASE("Topic filter validation", "[mqtt::protocol]") {
    SECTION("valid topic filters") {
        REQUIRE(isValidTopicFilter("test"));
        REQUIRE(isValidTopicFilter("test/topic"));
        REQUIRE(isValidTopicFilter("sensors/#"));
        REQUIRE(isValidTopicFilter("sensors/+/temperature"));
        REQUIRE(isValidTopicFilter("+/+/+"));
        REQUIRE(isValidTopicFilter("#"));
        REQUIRE(isValidTopicFilter("+"));
        REQUIRE(isValidTopicFilter("a/+/c/#"));
    }

    SECTION("invalid topic filters") {
        REQUIRE_FALSE(isValidTopicFilter(""));  // Empty
        REQUIRE_FALSE(isValidTopicFilter("test+"));  // + not at level boundary
        REQUIRE_FALSE(isValidTopicFilter("test#"));  // # not at level boundary
        REQUIRE_FALSE(isValidTopicFilter("sensors/#/more"));  // # not at end
        REQUIRE_FALSE(isValidTopicFilter("sensors/temp+"));  // + not alone in level
    }
}

TEST_CASE("Topic matching", "[mqtt::protocol]") {
    SECTION("exact match") {
        REQUIRE(topicMatchesFilter("test", "test"));
        REQUIRE(topicMatchesFilter("a/b/c", "a/b/c"));
        REQUIRE_FALSE(topicMatchesFilter("test", "test2"));
        REQUIRE_FALSE(topicMatchesFilter("a/b", "a/b/c"));
    }

    SECTION("single level wildcard") {
        REQUIRE(topicMatchesFilter("a/b/c", "a/+/c"));
        REQUIRE(topicMatchesFilter("sensors/room1/temperature", "sensors/+/temperature"));
        REQUIRE(topicMatchesFilter("a/b/c", "+/+/+"));
        REQUIRE_FALSE(topicMatchesFilter("a/b/c/d", "a/+/c"));
    }

    SECTION("multi level wildcard") {
        REQUIRE(topicMatchesFilter("a", "#"));
        REQUIRE(topicMatchesFilter("a/b/c", "#"));
        REQUIRE(topicMatchesFilter("sensors/room1/temperature", "sensors/#"));
        REQUIRE(topicMatchesFilter("sensors", "sensors/#"));
    }

    SECTION("combined wildcards") {
        REQUIRE(topicMatchesFilter("a/b/c/d", "a/+/c/#"));
        REQUIRE(topicMatchesFilter("a/x/c", "a/+/c/#"));
        REQUIRE(topicMatchesFilter("a/x/c/d/e/f", "a/+/c/#"));
    }
}

TEST_CASE("CONNECT packet building", "[mqtt::protocol]") {
    Config config;
    config.clientId = "test-client";
    config.keepAlive = std::chrono::seconds(60);
    config.cleanSession = true;

    SECTION("basic CONNECT") {
        auto packet = buildConnectPacket(config, ProtocolVersion::V3_1_1);
        REQUIRE(!packet.empty());
        REQUIRE(static_cast<std::uint8_t>(packet[0]) == 0x10);  // CONNECT packet type
    }

    SECTION("with username/password") {
        config.username = "user";
        config.password = "pass";

        auto packet = buildConnectPacket(config, ProtocolVersion::V3_1_1);
        REQUIRE(!packet.empty());
    }

    SECTION("with will message") {
        config.will = Will{"will/topic", {std::byte{0x01}, std::byte{0x02}}, QoS::AT_LEAST_ONCE, false};

        auto packet = buildConnectPacket(config, ProtocolVersion::V3_1_1);
        REQUIRE(!packet.empty());
    }
}

TEST_CASE("PUBLISH packet building", "[mqtt::protocol]") {
    std::vector<std::byte> payload = {std::byte{0x48}, std::byte{0x65}, std::byte{0x6C}, std::byte{0x6C}, std::byte{0x6F}};  // "Hello"

    SECTION("QoS 0") {
        auto packet = buildPublishPacket("test/topic", payload, QoS::AT_MOST_ONCE, false, false, 0);
        REQUIRE(!packet.empty());
        REQUIRE((static_cast<std::uint8_t>(packet[0]) & 0xF0) == 0x30);  // PUBLISH packet type
    }

    SECTION("QoS 1") {
        auto packet = buildPublishPacket("test/topic", payload, QoS::AT_LEAST_ONCE, false, false, 1);
        REQUIRE(!packet.empty());
        REQUIRE((static_cast<std::uint8_t>(packet[0]) & 0x06) == 0x02);  // QoS 1
    }

    SECTION("QoS 2 with retain") {
        auto packet = buildPublishPacket("test/topic", payload, QoS::EXACTLY_ONCE, true, false, 1);
        REQUIRE(!packet.empty());
        REQUIRE((static_cast<std::uint8_t>(packet[0]) & 0x07) == 0x05);  // QoS 2 + retain
    }
}

TEST_CASE("SUBSCRIBE packet building", "[mqtt::protocol]") {
    std::vector<Subscription> subs = {
        Subscription("sensors/#", QoS::AT_LEAST_ONCE),
        Subscription("commands/+", QoS::EXACTLY_ONCE)
    };

    auto packet = buildSubscribePacket(1, subs, ProtocolVersion::V3_1_1);
    REQUIRE(!packet.empty());
    REQUIRE((static_cast<std::uint8_t>(packet[0]) & 0xF0) == 0x80);  // SUBSCRIBE packet type
    REQUIRE((static_cast<std::uint8_t>(packet[0]) & 0x02) == 0x02);  // Fixed QoS 1
}

TEST_CASE("UNSUBSCRIBE packet building", "[mqtt::protocol]") {
    std::vector<std::string> topics = {"sensors/#", "commands/+"};

    auto packet = buildUnsubscribePacket(1, topics, ProtocolVersion::V3_1_1);
    REQUIRE(!packet.empty());
    REQUIRE((static_cast<std::uint8_t>(packet[0]) & 0xF0) == 0xA0);  // UNSUBSCRIBE packet type
}

TEST_CASE("PINGREQ packet building", "[mqtt::protocol]") {
    auto packet = buildPingreqPacket();
    REQUIRE(packet.size() == 2);
    REQUIRE(packet[0] == std::byte{0xC0});
    REQUIRE(packet[1] == std::byte{0x00});
}

TEST_CASE("DISCONNECT packet building", "[mqtt::protocol]") {
    SECTION("MQTT 3.1.1") {
        auto packet = buildDisconnectPacket(ProtocolVersion::V3_1_1);
        REQUIRE(packet.size() == 2);
        REQUIRE(packet[0] == std::byte{0xE0});
        REQUIRE(packet[1] == std::byte{0x00});
    }

    SECTION("MQTT 5.0") {
        auto packet = buildDisconnectPacket(ProtocolVersion::V5, ReasonCode::NORMAL_DISCONNECTION);
        REQUIRE(!packet.empty());
        REQUIRE((static_cast<std::uint8_t>(packet[0]) & 0xF0) == 0xE0);
    }
}

TEST_CASE("CONNACK packet parsing", "[mqtt::protocol]") {
    SECTION("successful connection") {
        std::array<std::byte, 2> data{std::byte{0x00}, std::byte{0x00}};
        auto result = parseConnackPacket(data, ProtocolVersion::V3_1_1);
        REQUIRE(result);
        REQUIRE(result->sessionPresent == false);
        REQUIRE(result->reasonCode == ReasonCode::SUCCESS);
    }

    SECTION("session present") {
        std::array<std::byte, 2> data{std::byte{0x01}, std::byte{0x00}};
        auto result = parseConnackPacket(data, ProtocolVersion::V3_1_1);
        REQUIRE(result);
        REQUIRE(result->sessionPresent == true);
        REQUIRE(result->reasonCode == ReasonCode::SUCCESS);
    }

    SECTION("connection refused") {
        std::array<std::byte, 2> data{std::byte{0x00}, std::byte{0x05}};  // Not authorized
        auto result = parseConnackPacket(data, ProtocolVersion::V3_1_1);
        REQUIRE(result);
        REQUIRE(result->reasonCode == static_cast<ReasonCode>(5));
    }
}
