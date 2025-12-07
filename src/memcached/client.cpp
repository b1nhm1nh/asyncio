#include <asyncio/memcached/client.h>
#include <asyncio/memcached/protocol.h>
#include <asyncio/net/stream.h>
#include <asyncio/net/tls.h>
#include <asyncio/stream.h>
#include <asyncio/buffer.h>
#include <map>
#include <regex>

namespace asyncio::memcached {

// ==================== Error Category ====================

const std::error_category &Client::ErrorCategory::instance() {
    return errorCategoryInstance<Client::ErrorCategory>();
}

using namespace protocol;

struct Client::Impl {
    std::shared_ptr<IReader> rawReader;
    std::shared_ptr<IBufReader> reader;  // Buffered reader for efficiency
    std::shared_ptr<IWriter> writer;
    std::shared_ptr<ICloseable> closeable;
    Config config;
    bool connected{false};
    std::uint32_t nextOpaque{1};

    task::Task<void, std::error_code> sendRequest(std::span<const std::byte> data) {
        CO_EXPECT(co_await writer->writeAll(data));
        co_return {};
    }

    task::Task<Response, std::error_code> readResponse() {
        // Read header (24 bytes)
        std::array<std::byte, 24> headerBuf;
        CO_EXPECT(co_await reader->readExactly(headerBuf));

        Header header = decodeHeader(headerBuf);

        if (header.magic != RESPONSE_MAGIC) {
            co_return std::unexpected{make_error_code(Error::SERVER_ERROR)};
        }

        // Read body
        std::vector<std::byte> body(header.totalBodyLength);
        if (header.totalBodyLength > 0) {
            CO_EXPECT(co_await reader->readExactly(body));
        }

        // Combine header + body for parsing
        std::vector<std::byte> fullPacket;
        fullPacket.reserve(24 + body.size());
        fullPacket.insert(fullPacket.end(), headerBuf.begin(), headerBuf.end());
        fullPacket.insert(fullPacket.end(), body.begin(), body.end());

        auto response = parseResponse(fullPacket);
        if (!response) {
            co_return std::unexpected{make_error_code(Error::SERVER_ERROR)};
        }

        co_return *response;
    }

    task::Task<Response, std::error_code> executeCommand(std::vector<std::byte> request) {
        CO_EXPECT(co_await sendRequest(request));
        co_return co_await readResponse();
    }
};

Client::Client() : mImpl(std::make_unique<Impl>()) {}
Client::~Client() = default;
Client::Client(Client &&) noexcept = default;
Client &Client::operator=(Client &&) noexcept = default;

task::Task<Client, std::error_code> Client::connect(Config config) {
    Client client;
    client.mImpl->config = std::move(config);

    // Connect to first server
    if (client.mImpl->config.servers.empty()) {
        co_return std::unexpected{make_error_code(Error::CONNECTION_FAILED)};
    }

    const auto &server = client.mImpl->config.servers[0];
    auto stream = co_await net::TCPStream::connect(server.host, server.port);
    if (!stream) {
        co_return std::unexpected{make_error_code(Error::CONNECTION_FAILED)};
    }

    auto streamPtr = std::make_shared<net::TCPStream>(std::move(*stream));
    client.mImpl->rawReader = streamPtr;
    // Use buffered reader for better mget performance (reduces syscalls)
    client.mImpl->reader = std::make_shared<BufReader<std::shared_ptr<IReader>>>(
        std::static_pointer_cast<IReader>(streamPtr), 16384);
    client.mImpl->writer = streamPtr;
    client.mImpl->closeable = streamPtr;
    client.mImpl->connected = true;

    co_return std::move(client);
}

task::Task<Client, std::error_code> Client::connect(std::string host, std::uint16_t port) {
    Config config;
    config.servers.emplace_back(std::move(host), port);
    co_return co_await connect(std::move(config));
}

task::Task<Client, std::error_code> Client::connect(std::string connectionString) {
    Config config;

    // Parse "host1:port1,host2:port2,..."
    std::regex serverRegex(R"(([^:,]+)(?::(\d+))?)");
    auto begin = std::sregex_iterator(connectionString.begin(), connectionString.end(), serverRegex);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string host = (*it)[1].str();
        std::uint16_t port = DEFAULT_PORT;
        if ((*it)[2].matched) {
            port = static_cast<std::uint16_t>(std::stoi((*it)[2].str()));
        }
        config.servers.emplace_back(std::move(host), port);
    }

    if (config.servers.empty()) {
        co_return std::unexpected{make_error_code(Error::INVALID_ARGUMENTS)};
    }

    co_return co_await connect(std::move(config));
}

bool Client::isConnected() const {
    return mImpl && mImpl->connected;
}

task::Task<void, std::error_code> Client::disconnect() {
    if (!mImpl || !mImpl->connected) {
        co_return {};
    }

    // Send QUIT command (don't wait for response)
    auto request = buildQuitRequest(mImpl->nextOpaque++);
    (void)co_await mImpl->sendRequest(request);

    mImpl->connected = false;
    if (mImpl->closeable) {
        co_await mImpl->closeable->close();
    }

    co_return {};
}

// ==================== Storage Commands ====================

task::Task<std::optional<Value>, std::error_code> Client::get(std::string_view key) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildGetRequest(key, mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status == Status::KEY_NOT_FOUND) {
        co_return std::nullopt;
    }

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    Value value;
    value.data = std::move(response->value);
    value.flags = response->flags;
    value.cas = response->cas;
    co_return value;
}

task::Task<std::optional<std::string>, std::error_code> Client::getString(std::string_view key) {
    auto result = co_await get(key);
    CO_EXPECT(result);

    if (!(*result).has_value()) {
        co_return std::nullopt;
    }

    co_return (*result)->asString();
}

task::Task<std::map<std::string, Value>, std::error_code>
Client::mget(std::span<const std::string> keys) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    if (keys.empty()) {
        co_return std::map<std::string, Value>{};
    }

    std::uint32_t startOpaque = mImpl->nextOpaque;
    mImpl->nextOpaque += static_cast<std::uint32_t>(keys.size()) + 1;

    auto request = buildMultiGetRequest(keys, startOpaque);
    CO_EXPECT(co_await mImpl->sendRequest(request));

    std::map<std::string, Value> results;
    std::uint32_t noopOpaque = startOpaque + static_cast<std::uint32_t>(keys.size());

    // Read responses until we get the NOOP response
    while (true) {
        auto response = co_await mImpl->readResponse();
        CO_EXPECT(response);

        if (response->opcode == Opcode::NOOP && response->opaque == noopOpaque) {
            break;  // All responses received
        }

        if (response->status == Status::NO_ERROR && !response->key.empty()) {
            Value value;
            value.data = std::move(response->value);
            value.flags = response->flags;
            value.cas = response->cas;
            results[response->key] = std::move(value);
        }
    }

    co_return results;
}

task::Task<void, std::error_code> Client::set(
    std::string_view key,
    std::span<const std::byte> value,
    std::chrono::seconds exptime,
    std::uint32_t flags
) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildSetRequest(key, value, flags,
                                   static_cast<std::uint32_t>(exptime.count()),
                                   0, mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    co_return {};
}

task::Task<void, std::error_code> Client::set(
    std::string_view key,
    std::string_view value,
    std::chrono::seconds exptime,
    std::uint32_t flags
) {
    auto bytes = std::span{reinterpret_cast<const std::byte*>(value.data()), value.size()};
    co_return co_await set(key, bytes, exptime, flags);
}

task::Task<bool, std::error_code> Client::add(
    std::string_view key,
    std::span<const std::byte> value,
    std::chrono::seconds exptime,
    std::uint32_t flags
) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildAddRequest(key, value, flags,
                                   static_cast<std::uint32_t>(exptime.count()),
                                   mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status == Status::KEY_EXISTS) {
        co_return false;
    }

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    co_return true;
}

task::Task<bool, std::error_code> Client::add(
    std::string_view key,
    std::string_view value,
    std::chrono::seconds exptime,
    std::uint32_t flags
) {
    auto bytes = std::span{reinterpret_cast<const std::byte*>(value.data()), value.size()};
    co_return co_await add(key, bytes, exptime, flags);
}

task::Task<bool, std::error_code> Client::replace(
    std::string_view key,
    std::span<const std::byte> value,
    std::chrono::seconds exptime,
    std::uint32_t flags
) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildReplaceRequest(key, value, flags,
                                       static_cast<std::uint32_t>(exptime.count()),
                                       0, mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status == Status::KEY_NOT_FOUND) {
        co_return false;
    }

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    co_return true;
}

task::Task<bool, std::error_code> Client::replace(
    std::string_view key,
    std::string_view value,
    std::chrono::seconds exptime,
    std::uint32_t flags
) {
    auto bytes = std::span{reinterpret_cast<const std::byte*>(value.data()), value.size()};
    co_return co_await replace(key, bytes, exptime, flags);
}

task::Task<void, std::error_code> Client::append(std::string_view key, std::string_view value) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto bytes = std::span{reinterpret_cast<const std::byte*>(value.data()), value.size()};
    auto request = buildAppendRequest(key, bytes, 0, mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    co_return {};
}

task::Task<void, std::error_code> Client::prepend(std::string_view key, std::string_view value) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto bytes = std::span{reinterpret_cast<const std::byte*>(value.data()), value.size()};
    auto request = buildPrependRequest(key, bytes, 0, mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    co_return {};
}

task::Task<bool, std::error_code> Client::cas(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint64_t casUnique,
    std::chrono::seconds exptime,
    std::uint32_t flags
) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildSetRequest(key, value, flags,
                                   static_cast<std::uint32_t>(exptime.count()),
                                   casUnique, mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status == Status::KEY_EXISTS) {
        co_return false;  // CAS mismatch
    }

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    co_return true;
}

// ==================== Delete Commands ====================

task::Task<bool, std::error_code> Client::del(std::string_view key) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildDeleteRequest(key, mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status == Status::KEY_NOT_FOUND) {
        co_return false;
    }

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    co_return true;
}

task::Task<std::int64_t, std::error_code> Client::del(std::span<const std::string> keys) {
    std::int64_t count = 0;
    for (const auto &key : keys) {
        auto result = co_await del(key);
        CO_EXPECT(result);
        if (*result) {
            ++count;
        }
    }
    co_return count;
}

// ==================== Numeric Commands ====================

task::Task<std::uint64_t, std::error_code> Client::incr(std::string_view key, std::uint64_t delta) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildIncrRequest(key, delta, 0, 0xFFFFFFFF, mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    // Value is the new counter value as 64-bit big-endian
    if (response->value.size() >= 8) {
        std::array<std::byte, 8> buf;
        std::copy_n(response->value.begin(), 8, buf.begin());
        co_return decodeUint64(buf);
    }

    co_return std::unexpected{make_error_code(Error::SERVER_ERROR)};
}

task::Task<std::uint64_t, std::error_code> Client::decr(std::string_view key, std::uint64_t delta) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildDecrRequest(key, delta, 0, 0xFFFFFFFF, mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    if (response->value.size() >= 8) {
        std::array<std::byte, 8> buf;
        std::copy_n(response->value.begin(), 8, buf.begin());
        co_return decodeUint64(buf);
    }

    co_return std::unexpected{make_error_code(Error::SERVER_ERROR)};
}

// ==================== Touch Commands ====================

task::Task<bool, std::error_code> Client::touch(std::string_view key, std::chrono::seconds exptime) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildTouchRequest(key, static_cast<std::uint32_t>(exptime.count()),
                                     mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status == Status::KEY_NOT_FOUND) {
        co_return false;
    }

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    co_return true;
}

task::Task<std::optional<Value>, std::error_code>
Client::gat(std::string_view key, std::chrono::seconds exptime) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildGatRequest(key, static_cast<std::uint32_t>(exptime.count()),
                                   mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status == Status::KEY_NOT_FOUND) {
        co_return std::nullopt;
    }

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    Value value;
    value.data = std::move(response->value);
    value.flags = response->flags;
    value.cas = response->cas;
    co_return value;
}

// ==================== Server Commands ====================

task::Task<void, std::error_code> Client::flushAll(std::chrono::seconds delay) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildFlushRequest(static_cast<std::uint32_t>(delay.count()),
                                     mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    co_return {};
}

task::Task<std::vector<ServerStats>, std::error_code> Client::stats(std::string_view type) {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildStatRequest(type, mImpl->nextOpaque++);
    CO_EXPECT(co_await mImpl->sendRequest(request));

    std::vector<ServerStats> allStats;
    ServerStats currentStats;
    currentStats.server = mImpl->config.servers[0].host + ":" +
                          std::to_string(mImpl->config.servers[0].port);

    // Read stat responses until we get an empty key response
    while (true) {
        auto response = co_await mImpl->readResponse();
        CO_EXPECT(response);

        if (response->key.empty()) {
            break;  // End of stats
        }

        currentStats.stats[response->key] = std::string(
            reinterpret_cast<const char*>(response->value.data()),
            response->value.size()
        );
    }

    allStats.push_back(std::move(currentStats));
    co_return allStats;
}

task::Task<std::map<std::string, std::string>, std::error_code> Client::version() {
    if (!isConnected()) {
        co_return std::unexpected{make_error_code(Error::NOT_CONNECTED)};
    }

    auto request = buildVersionRequest(mImpl->nextOpaque++);
    auto response = co_await mImpl->executeCommand(std::move(request));
    CO_EXPECT(response);

    if (response->status != Status::NO_ERROR) {
        co_return std::unexpected{statusToError(response->status)};
    }

    std::map<std::string, std::string> versions;
    versions[mImpl->config.servers[0].host + ":" +
             std::to_string(mImpl->config.servers[0].port)] =
        std::string(reinterpret_cast<const char*>(response->value.data()),
                    response->value.size());

    co_return versions;
}

task::Task<void, std::error_code> Client::quit() {
    co_return co_await disconnect();
}

}  // namespace asyncio::memcached
