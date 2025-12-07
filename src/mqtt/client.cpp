#include <asyncio/mqtt/client.h>
#include <asyncio/mqtt/protocol.h>
#include <asyncio/net/stream.h>
#include <asyncio/net/tls.h>
#include <asyncio/time.h>
#include <asyncio/buffer.h>
#include <map>
#include <random>
#include <regex>

namespace asyncio::mqtt {

// ==================== ReasonCode Category ====================

const std::error_category &ReasonCodeCategory::instance() {
    return errorCategoryInstance<ReasonCodeCategory>();
}

std::string ReasonCodeCategory::message(int value) const {
    switch (static_cast<ReasonCode>(value)) {
        case ReasonCode::SUCCESS: return "success";
        case ReasonCode::GRANTED_QOS_1: return "granted qos 1";
        case ReasonCode::GRANTED_QOS_2: return "granted qos 2";
        case ReasonCode::DISCONNECT_WITH_WILL: return "disconnect with will message";
        case ReasonCode::NO_MATCHING_SUBSCRIBERS: return "no matching subscribers";
        case ReasonCode::NO_SUBSCRIPTION_EXISTED: return "no subscription existed";
        case ReasonCode::CONTINUE_AUTHENTICATION: return "continue authentication";
        case ReasonCode::RE_AUTHENTICATE: return "re-authenticate";
        case ReasonCode::UNSPECIFIED_ERROR: return "unspecified error";
        case ReasonCode::MALFORMED_PACKET: return "malformed packet";
        case ReasonCode::PROTOCOL_ERROR: return "protocol error";
        case ReasonCode::IMPLEMENTATION_SPECIFIC_ERROR: return "implementation specific error";
        case ReasonCode::UNSUPPORTED_PROTOCOL_VERSION: return "unsupported protocol version";
        case ReasonCode::CLIENT_IDENTIFIER_NOT_VALID: return "client identifier not valid";
        case ReasonCode::BAD_USERNAME_OR_PASSWORD: return "bad username or password";
        case ReasonCode::NOT_AUTHORIZED: return "not authorized";
        case ReasonCode::SERVER_UNAVAILABLE: return "server unavailable";
        case ReasonCode::SERVER_BUSY: return "server busy";
        case ReasonCode::BANNED: return "banned";
        case ReasonCode::SERVER_SHUTTING_DOWN: return "server shutting down";
        case ReasonCode::BAD_AUTHENTICATION_METHOD: return "bad authentication method";
        case ReasonCode::KEEP_ALIVE_TIMEOUT: return "keep alive timeout";
        case ReasonCode::SESSION_TAKEN_OVER: return "session taken over";
        case ReasonCode::TOPIC_FILTER_INVALID: return "topic filter invalid";
        case ReasonCode::TOPIC_NAME_INVALID: return "topic name invalid";
        case ReasonCode::PACKET_IDENTIFIER_IN_USE: return "packet identifier in use";
        case ReasonCode::PACKET_IDENTIFIER_NOT_FOUND: return "packet identifier not found";
        case ReasonCode::RECEIVE_MAXIMUM_EXCEEDED: return "receive maximum exceeded";
        case ReasonCode::TOPIC_ALIAS_INVALID: return "topic alias invalid";
        case ReasonCode::PACKET_TOO_LARGE: return "packet too large";
        case ReasonCode::MESSAGE_RATE_TOO_HIGH: return "message rate too high";
        case ReasonCode::QUOTA_EXCEEDED: return "quota exceeded";
        case ReasonCode::ADMINISTRATIVE_ACTION: return "administrative action";
        case ReasonCode::PAYLOAD_FORMAT_INVALID: return "payload format invalid";
        case ReasonCode::RETAIN_NOT_SUPPORTED: return "retain not supported";
        case ReasonCode::QOS_NOT_SUPPORTED: return "qos not supported";
        case ReasonCode::USE_ANOTHER_SERVER: return "use another server";
        case ReasonCode::SERVER_MOVED: return "server moved";
        case ReasonCode::SHARED_SUBSCRIPTIONS_NOT_SUPPORTED: return "shared subscriptions not supported";
        case ReasonCode::CONNECTION_RATE_EXCEEDED: return "connection rate exceeded";
        case ReasonCode::MAXIMUM_CONNECT_TIME: return "maximum connect time";
        case ReasonCode::SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED: return "subscription identifiers not supported";
        case ReasonCode::WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED: return "wildcard subscriptions not supported";
        default: return "unknown reason code";
    }
}

// ==================== Client Error Category ====================

const std::error_category &Client::ErrorCategory::instance() {
    return errorCategoryInstance<Client::ErrorCategory>();
}

// ==================== URL Parsing ====================

std::expected<URL, std::error_code> URL::parse(std::string_view urlStr) {
    URL url;

    // Simple URL parsing: scheme://host:port/path
    std::string urlString(urlStr);
    std::regex urlRegex(R"(^(mqtt|mqtts)://([^:/]+)(?::(\d+))?(/.*)?$)");
    std::smatch match;

    if (!std::regex_match(urlString, match, urlRegex)) {
        return std::unexpected(make_error_code(Client::Error::INVALID_URL));
    }

    url.scheme = match[1].str();
    url.host = match[2].str();

    if (match[3].matched) {
        url.port = static_cast<std::uint16_t>(std::stoi(match[3].str()));
    } else {
        url.port = (url.scheme == "mqtts") ? DEFAULT_TLS_PORT : DEFAULT_PORT;
    }

    if (match[4].matched) {
        url.path = match[4].str();
    }

    return url;
}

// ==================== Client Implementation ====================

struct Client::Impl {
    Config config;
    ProtocolVersion protocolVersion;
    State state = State::DISCONNECTED;

    std::shared_ptr<IReader> reader;
    std::shared_ptr<IWriter> writer;
    std::shared_ptr<ICloseable> closeable;

    std::unique_ptr<sync::Mutex> writeMutex;

    // Packet ID management
    std::uint16_t nextPacketId = 1;
    std::set<std::uint16_t> usedPacketIds;

    // Message channel for received PUBLISH messages
    std::optional<Channel<Message>> messageChannel;

    // Keep-alive timer task
    std::optional<task::Task<void, std::error_code>> keepAliveTask;
    bool pingPending = false;

    // Read loop task
    std::optional<task::Task<void, std::error_code>> readLoopTask;

    Impl(Config cfg)
        : config(std::move(cfg))
        , protocolVersion(config.protocolVersion)
        , writeMutex(std::make_unique<sync::Mutex>()) {}

    std::uint16_t allocatePacketId() {
        for (int i = 0; i < 65535; ++i) {
            std::uint16_t id = nextPacketId++;
            if (nextPacketId == 0) nextPacketId = 1;
            if (usedPacketIds.find(id) == usedPacketIds.end()) {
                usedPacketIds.insert(id);
                return id;
            }
        }
        return 0;  // All IDs exhausted
    }

    void releasePacketId(std::uint16_t id) {
        usedPacketIds.erase(id);
    }

    task::Task<void, std::error_code> writePacket(std::vector<std::byte> packet) {
        CO_EXPECT(co_await writeMutex->lock());
        auto writeResult = co_await writer->writeAll(packet);
        writeMutex->unlock();
        CO_EXPECT(writeResult);
        co_return {};
    }

    task::Task<protocol::FixedHeader, std::error_code> readFixedHeader() {
        std::array<std::byte, 5> headerBuf;
        std::size_t headerLen = 0;

        // Read first byte
        CO_EXPECT(co_await reader->readExactly(std::span(headerBuf).subspan(0, 1)));
        headerLen = 1;

        // Read remaining length (variable byte integer)
        do {
            CO_EXPECT(co_await reader->readExactly(std::span(headerBuf).subspan(headerLen, 1)));
            headerLen++;
        } while ((static_cast<std::uint8_t>(headerBuf[headerLen - 1]) & 0x80) && headerLen < 5);

        auto result = protocol::decodeFixedHeader(std::span(headerBuf).subspan(0, headerLen));
        if (!result) {
            co_return std::unexpected(result.error());
        }

        co_return result->first;
    }

    task::Task<std::vector<std::byte>, std::error_code> readPacketBody(std::uint32_t length) {
        std::vector<std::byte> body(length);
        if (length > 0) {
            CO_EXPECT(co_await reader->readExactly(body));
        }
        co_return body;
    }

    task::Task<void, std::error_code> handlePacket(const protocol::FixedHeader &header, std::span<const std::byte> body) {
        switch (header.type) {
            case PacketType::CONNACK:
                // Handled in connect()
                break;

            case PacketType::PUBLISH: {
                auto result = protocol::parsePublishPacket(header, body, protocolVersion);
                if (!result) {
                    co_return std::unexpected(result.error());
                }

                Message msg;
                msg.topic = std::move(result->topic);
                msg.payload = std::move(result->payload);
                msg.qos = result->qos;
                msg.retain = result->retain;
                msg.dup = result->dup;
                msg.packetId = result->packetId;

                // Send acknowledgment based on QoS
                if (msg.qos == QoS::AT_LEAST_ONCE) {
                    auto puback = protocol::buildPubackPacket(msg.packetId, protocolVersion);
                    CO_EXPECT(co_await writePacket(std::move(puback)));
                } else if (msg.qos == QoS::EXACTLY_ONCE) {
                    auto pubrec = protocol::buildPubrecPacket(msg.packetId, protocolVersion);
                    CO_EXPECT(co_await writePacket(std::move(pubrec)));
                }

                // Deliver message to channel
                if (messageChannel) {
                    auto &[sender, _] = *messageChannel;
                    sender.trySend(std::move(msg));
                }
                break;
            }

            case PacketType::PUBACK:
            case PacketType::PUBCOMP:
                // Handled synchronously in publish()
                break;

            case PacketType::PUBREC: {
                // Send PUBREL for QoS 2
                auto result = protocol::parseAckPacket(body, protocolVersion);
                if (!result) {
                    co_return std::unexpected(result.error());
                }
                auto pubrel = protocol::buildPubrelPacket(result->packetId, protocolVersion);
                CO_EXPECT(co_await writePacket(std::move(pubrel)));
                break;
            }

            case PacketType::PUBREL: {
                auto result = protocol::parseAckPacket(body, protocolVersion);
                if (!result) {
                    co_return std::unexpected(result.error());
                }
                // Send PUBCOMP
                auto pubcomp = protocol::buildPubcompPacket(result->packetId, protocolVersion);
                CO_EXPECT(co_await writePacket(std::move(pubcomp)));
                break;
            }

            case PacketType::SUBACK: {
                // Handled by subscribe() waiting on packet ID
                break;
            }

            case PacketType::UNSUBACK: {
                // Handled by unsubscribe() waiting on packet ID
                break;
            }

            case PacketType::PINGRESP:
                pingPending = false;
                break;

            case PacketType::DISCONNECT:
                state = State::DISCONNECTED;
                break;

            default:
                break;
        }

        co_return {};
    }

    task::Task<void, std::error_code> readLoop() {
        while (state == State::CONNECTED) {
            auto headerResult = co_await readFixedHeader();
            if (!headerResult) {
                if (state != State::CONNECTED) {
                    co_return {};  // Normal shutdown
                }
                co_return std::unexpected(headerResult.error());
            }

            auto bodyResult = co_await readPacketBody(headerResult->remainingLength);
            if (!bodyResult) {
                co_return std::unexpected(bodyResult.error());
            }

            auto handleResult = co_await handlePacket(*headerResult, *bodyResult);
            if (!handleResult) {
                co_return std::unexpected(handleResult.error());
            }
        }
        co_return {};
    }

    task::Task<void, std::error_code> keepAliveLoop() {
        while (state == State::CONNECTED) {
            co_await sleep(config.keepAlive);

            if (state != State::CONNECTED) {
                break;
            }

            if (pingPending) {
                // No response to previous ping - connection dead
                state = State::DISCONNECTED;
                co_return std::unexpected(make_error_code(Error::TIMEOUT));
            }

            // Send PINGREQ
            auto pingreq = protocol::buildPingreqPacket();
            auto result = co_await writePacket(std::move(pingreq));
            if (!result) {
                co_return std::unexpected(result.error());
            }
            pingPending = true;
        }
        co_return {};
    }
};

// ==================== Client Public Methods ====================

Client::Client(Config config)
    : mImpl(std::make_unique<Impl>(std::move(config))) {}

Client::~Client() = default;

Client::Client(Client &&) noexcept = default;
Client &Client::operator=(Client &&) noexcept = default;

bool Client::isConnected() const {
    return mImpl->state == State::CONNECTED;
}

const std::string &Client::clientId() const {
    return mImpl->config.clientId;
}

task::Task<Client, std::error_code>
Client::connect(std::string host, std::uint16_t port, Config config) {
    // Generate client ID if not provided
    if (config.clientId.empty()) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 35);
        const char *chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        config.clientId = "asyncio-mqtt-";
        for (int i = 0; i < 10; ++i) {
            config.clientId += chars[dis(gen)];
        }
    }

    // Connect TCP
    auto streamResult = co_await net::TCPStream::connect(std::move(host), port);
    if (!streamResult) {
        co_return std::unexpected(streamResult.error());
    }

    auto stream = std::make_shared<net::TCPStream>(std::move(*streamResult));

    Client client(std::move(config));
    client.mImpl->reader = stream;
    client.mImpl->writer = stream;
    client.mImpl->closeable = stream;
    client.mImpl->state = State::CONNECTING;

    // Send CONNECT packet
    auto connectPacket = protocol::buildConnectPacket(
        client.mImpl->config,
        client.mImpl->protocolVersion
    );

    CO_EXPECT(co_await client.mImpl->writePacket(std::move(connectPacket)));

    // Read CONNACK
    auto headerResult = co_await client.mImpl->readFixedHeader();
    if (!headerResult) {
        co_return std::unexpected(headerResult.error());
    }

    if (headerResult->type != PacketType::CONNACK) {
        co_return std::unexpected(make_error_code(Error::UNEXPECTED_PACKET));
    }

    auto bodyResult = co_await client.mImpl->readPacketBody(headerResult->remainingLength);
    if (!bodyResult) {
        co_return std::unexpected(bodyResult.error());
    }

    auto connackResult = protocol::parseConnackPacket(*bodyResult, client.mImpl->protocolVersion);
    if (!connackResult) {
        co_return std::unexpected(connackResult.error());
    }

    if (connackResult->reasonCode != ReasonCode::SUCCESS) {
        co_return std::unexpected(make_error_code(connackResult->reasonCode));
    }

    // Setup message channel
    client.mImpl->messageChannel = channel<Message>(100);

    // Start read loop and keep-alive
    client.mImpl->state = State::CONNECTED;

    co_return std::move(client);
}

task::Task<Client, std::error_code>
Client::connect(std::string host, std::uint16_t port, Config config, TLSConfig tlsConfig) {
    // Generate client ID if not provided
    if (config.clientId.empty()) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 35);
        const char *chars = "0123456789abcdefghijklmnopqrstuvwxyz";
        config.clientId = "asyncio-mqtt-";
        for (int i = 0; i < 10; ++i) {
            config.clientId += chars[dis(gen)];
        }
    }

    // Connect TCP
    auto tcpResult = co_await net::TCPStream::connect(host, port);
    if (!tcpResult) {
        co_return std::unexpected(tcpResult.error());
    }

    // Create TLS context
    auto context = net::tls::ClientConfig().build();
    if (!context) {
        co_return std::unexpected(context.error());
    }

    // Wrap with TLS
    auto tlsResult = co_await net::tls::connect(
        std::move(*tcpResult),
        *std::move(context),
        host
    );
    if (!tlsResult) {
        co_return std::unexpected(tlsResult.error());
    }

    auto stream = std::make_shared<net::tls::TLS<net::TCPStream>>(std::move(*tlsResult));

    Client client(std::move(config));
    client.mImpl->reader = stream;
    client.mImpl->writer = stream;
    client.mImpl->closeable = stream;
    client.mImpl->state = State::CONNECTING;

    // Send CONNECT packet
    auto connectPacket = protocol::buildConnectPacket(
        client.mImpl->config,
        client.mImpl->protocolVersion
    );

    CO_EXPECT(co_await client.mImpl->writePacket(std::move(connectPacket)));

    // Read CONNACK
    auto headerResult = co_await client.mImpl->readFixedHeader();
    if (!headerResult) {
        co_return std::unexpected(headerResult.error());
    }

    if (headerResult->type != PacketType::CONNACK) {
        co_return std::unexpected(make_error_code(Error::UNEXPECTED_PACKET));
    }

    auto bodyResult = co_await client.mImpl->readPacketBody(headerResult->remainingLength);
    if (!bodyResult) {
        co_return std::unexpected(bodyResult.error());
    }

    auto connackResult = protocol::parseConnackPacket(*bodyResult, client.mImpl->protocolVersion);
    if (!connackResult) {
        co_return std::unexpected(connackResult.error());
    }

    if (connackResult->reasonCode != ReasonCode::SUCCESS) {
        co_return std::unexpected(make_error_code(connackResult->reasonCode));
    }

    // Setup message channel
    client.mImpl->messageChannel = channel<Message>(100);
    client.mImpl->state = State::CONNECTED;

    co_return std::move(client);
}

task::Task<Client, std::error_code>
Client::connect(std::string url, Config config) {
    auto urlResult = URL::parse(url);
    if (!urlResult) {
        co_return std::unexpected(urlResult.error());
    }

    if (urlResult->scheme == "mqtts") {
        co_return co_await connect(urlResult->host, urlResult->port, std::move(config), TLSConfig{});
    }

    co_return co_await connect(urlResult->host, urlResult->port, std::move(config));
}

task::Task<std::uint16_t, std::error_code>
Client::publish(std::string topic, std::span<const std::byte> payload, QoS qos, bool retain) {
    if (mImpl->state != State::CONNECTED) {
        co_return std::unexpected(make_error_code(Error::NOT_CONNECTED));
    }

    if (!protocol::isValidTopicName(topic)) {
        co_return std::unexpected(make_error_code(Error::INVALID_TOPIC));
    }

    std::uint16_t packetId = 0;
    if (qos != QoS::AT_MOST_ONCE) {
        packetId = mImpl->allocatePacketId();
        if (packetId == 0) {
            co_return std::unexpected(make_error_code(Error::PACKET_ID_EXHAUSTED));
        }
    }

    auto packet = protocol::buildPublishPacket(
        topic, payload, qos, retain, false, packetId, mImpl->protocolVersion
    );

    CO_EXPECT(co_await mImpl->writePacket(std::move(packet)));

    if (qos == QoS::AT_MOST_ONCE) {
        co_return 0;
    }

    // Wait for acknowledgment - read packets until we get our ACK
    while (true) {
        auto headerResult = co_await mImpl->readFixedHeader();
        if (!headerResult) {
            mImpl->releasePacketId(packetId);
            co_return std::unexpected(headerResult.error());
        }

        auto bodyResult = co_await mImpl->readPacketBody(headerResult->remainingLength);
        if (!bodyResult) {
            mImpl->releasePacketId(packetId);
            co_return std::unexpected(bodyResult.error());
        }

        // Handle the packet
        CO_EXPECT(co_await mImpl->handlePacket(*headerResult, *bodyResult));

        // Check if it's our ACK
        if ((qos == QoS::AT_LEAST_ONCE && headerResult->type == PacketType::PUBACK) ||
            (qos == QoS::EXACTLY_ONCE && headerResult->type == PacketType::PUBCOMP)) {
            auto ackResult = protocol::parseAckPacket(*bodyResult, mImpl->protocolVersion);
            if (ackResult && ackResult->packetId == packetId) {
                mImpl->releasePacketId(packetId);
                co_return packetId;
            }
        }
    }
}

task::Task<std::uint16_t, std::error_code>
Client::publish(std::string topic, std::string payload, QoS qos, bool retain) {
    std::vector<std::byte> bytes(payload.size());
    std::memcpy(bytes.data(), payload.data(), payload.size());
    co_return co_await publish(std::move(topic), bytes, qos, retain);
}

task::Task<std::vector<SubscribeResult>, std::error_code>
Client::subscribe(std::vector<Subscription> subscriptions) {
    if (mImpl->state != State::CONNECTED) {
        co_return std::unexpected(make_error_code(Error::NOT_CONNECTED));
    }

    for (const auto &sub : subscriptions) {
        if (!protocol::isValidTopicFilter(sub.topicFilter)) {
            co_return std::unexpected(make_error_code(Error::INVALID_TOPIC));
        }
    }

    std::uint16_t packetId = mImpl->allocatePacketId();
    if (packetId == 0) {
        co_return std::unexpected(make_error_code(Error::PACKET_ID_EXHAUSTED));
    }

    auto packet = protocol::buildSubscribePacket(packetId, subscriptions, mImpl->protocolVersion);
    CO_EXPECT(co_await mImpl->writePacket(std::move(packet)));

    // Read SUBACK
    auto headerResult = co_await mImpl->readFixedHeader();
    if (!headerResult) {
        mImpl->releasePacketId(packetId);
        co_return std::unexpected(headerResult.error());
    }

    if (headerResult->type != PacketType::SUBACK) {
        mImpl->releasePacketId(packetId);
        co_return std::unexpected(make_error_code(Error::UNEXPECTED_PACKET));
    }

    auto bodyResult = co_await mImpl->readPacketBody(headerResult->remainingLength);
    if (!bodyResult) {
        mImpl->releasePacketId(packetId);
        co_return std::unexpected(bodyResult.error());
    }

    auto subackResult = protocol::parseSubackPacket(*bodyResult, mImpl->protocolVersion);
    mImpl->releasePacketId(packetId);

    if (!subackResult) {
        co_return std::unexpected(subackResult.error());
    }

    std::vector<SubscribeResult> results;
    for (const auto &rc : subackResult->reasonCodes) {
        SubscribeResult result;
        result.reasonCode = rc;
        result.grantedQos = static_cast<QoS>(static_cast<std::uint8_t>(rc) & 0x03);
        results.push_back(result);
    }

    co_return results;
}

task::Task<SubscribeResult, std::error_code>
Client::subscribe(std::string topicFilter, QoS qos) {
    auto results = co_await subscribe(std::vector<Subscription>{{std::move(topicFilter), qos}});
    if (!results) {
        co_return std::unexpected(results.error());
    }
    if (results->empty()) {
        co_return std::unexpected(make_error_code(Error::PROTOCOL_ERROR));
    }
    co_return (*results)[0];
}

task::Task<void, std::error_code>
Client::unsubscribe(std::vector<std::string> topics) {
    if (mImpl->state != State::CONNECTED) {
        co_return std::unexpected(make_error_code(Error::NOT_CONNECTED));
    }

    std::uint16_t packetId = mImpl->allocatePacketId();
    if (packetId == 0) {
        co_return std::unexpected(make_error_code(Error::PACKET_ID_EXHAUSTED));
    }

    auto packet = protocol::buildUnsubscribePacket(packetId, topics, mImpl->protocolVersion);
    CO_EXPECT(co_await mImpl->writePacket(std::move(packet)));

    // Read UNSUBACK
    auto headerResult = co_await mImpl->readFixedHeader();
    if (!headerResult) {
        mImpl->releasePacketId(packetId);
        co_return std::unexpected(headerResult.error());
    }

    if (headerResult->type != PacketType::UNSUBACK) {
        mImpl->releasePacketId(packetId);
        co_return std::unexpected(make_error_code(Error::UNEXPECTED_PACKET));
    }

    auto bodyResult = co_await mImpl->readPacketBody(headerResult->remainingLength);
    mImpl->releasePacketId(packetId);

    if (!bodyResult) {
        co_return std::unexpected(bodyResult.error());
    }

    co_return {};
}

task::Task<void, std::error_code>
Client::unsubscribe(std::string topicFilter) {
    co_return co_await unsubscribe(std::vector<std::string>{std::move(topicFilter)});
}

task::Task<Message, std::error_code>
Client::receive() {
    if (!mImpl->messageChannel) {
        co_return std::unexpected(make_error_code(Error::NOT_CONNECTED));
    }

    // First process any pending packets
    while (mImpl->state == State::CONNECTED) {
        // Try to receive from channel first
        auto &[_, receiver] = *mImpl->messageChannel;
        auto tryResult = receiver.tryReceive();
        if (tryResult) {
            co_return std::move(*tryResult);
        }

        // Read next packet
        auto headerResult = co_await mImpl->readFixedHeader();
        if (!headerResult) {
            co_return std::unexpected(headerResult.error());
        }

        auto bodyResult = co_await mImpl->readPacketBody(headerResult->remainingLength);
        if (!bodyResult) {
            co_return std::unexpected(bodyResult.error());
        }

        CO_EXPECT(co_await mImpl->handlePacket(*headerResult, *bodyResult));

        // Check channel again after handling
        tryResult = receiver.tryReceive();
        if (tryResult) {
            co_return std::move(*tryResult);
        }
    }

    co_return std::unexpected(make_error_code(Error::NOT_CONNECTED));
}

task::Task<void, std::error_code>
Client::disconnect(ReasonCode reasonCode) {
    if (mImpl->state != State::CONNECTED) {
        co_return std::unexpected(make_error_code(Error::NOT_CONNECTED));
    }

    mImpl->state = State::DISCONNECTING;

    auto packet = protocol::buildDisconnectPacket(mImpl->protocolVersion, reasonCode);
    CO_EXPECT(co_await mImpl->writePacket(std::move(packet)));

    // Close connection
    CO_EXPECT(co_await mImpl->closeable->close());

    mImpl->state = State::DISCONNECTED;
    co_return {};
}

} // namespace asyncio::mqtt
