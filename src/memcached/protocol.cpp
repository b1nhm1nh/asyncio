#include <asyncio/memcached/protocol.h>
#include <cstring>
#include <bit>

namespace asyncio::memcached::protocol {

// ==================== Encoding Functions ====================

void encodeUint16(std::uint16_t value, std::span<std::byte, 2> buffer) {
    buffer[0] = static_cast<std::byte>((value >> 8) & 0xFF);
    buffer[1] = static_cast<std::byte>(value & 0xFF);
}

void encodeUint32(std::uint32_t value, std::span<std::byte, 4> buffer) {
    buffer[0] = static_cast<std::byte>((value >> 24) & 0xFF);
    buffer[1] = static_cast<std::byte>((value >> 16) & 0xFF);
    buffer[2] = static_cast<std::byte>((value >> 8) & 0xFF);
    buffer[3] = static_cast<std::byte>(value & 0xFF);
}

void encodeUint64(std::uint64_t value, std::span<std::byte, 8> buffer) {
    buffer[0] = static_cast<std::byte>((value >> 56) & 0xFF);
    buffer[1] = static_cast<std::byte>((value >> 48) & 0xFF);
    buffer[2] = static_cast<std::byte>((value >> 40) & 0xFF);
    buffer[3] = static_cast<std::byte>((value >> 32) & 0xFF);
    buffer[4] = static_cast<std::byte>((value >> 24) & 0xFF);
    buffer[5] = static_cast<std::byte>((value >> 16) & 0xFF);
    buffer[6] = static_cast<std::byte>((value >> 8) & 0xFF);
    buffer[7] = static_cast<std::byte>(value & 0xFF);
}

void encodeHeader(const Header &header, std::span<std::byte, 24> buffer) {
    buffer[0] = static_cast<std::byte>(header.magic);
    buffer[1] = static_cast<std::byte>(header.opcode);
    encodeUint16(header.keyLength, buffer.subspan<2, 2>());
    buffer[4] = static_cast<std::byte>(header.extrasLength);
    buffer[5] = static_cast<std::byte>(header.dataType);
    encodeUint16(header.vbucketOrStatus, buffer.subspan<6, 2>());
    encodeUint32(header.totalBodyLength, buffer.subspan<8, 4>());
    encodeUint32(header.opaque, buffer.subspan<12, 4>());
    encodeUint64(header.cas, buffer.subspan<16, 8>());
}

// ==================== Decoding Functions ====================

std::uint16_t decodeUint16(std::span<const std::byte, 2> buffer) {
    return (static_cast<std::uint16_t>(buffer[0]) << 8) |
           static_cast<std::uint16_t>(buffer[1]);
}

std::uint32_t decodeUint32(std::span<const std::byte, 4> buffer) {
    return (static_cast<std::uint32_t>(buffer[0]) << 24) |
           (static_cast<std::uint32_t>(buffer[1]) << 16) |
           (static_cast<std::uint32_t>(buffer[2]) << 8) |
           static_cast<std::uint32_t>(buffer[3]);
}

std::uint64_t decodeUint64(std::span<const std::byte, 8> buffer) {
    return (static_cast<std::uint64_t>(buffer[0]) << 56) |
           (static_cast<std::uint64_t>(buffer[1]) << 48) |
           (static_cast<std::uint64_t>(buffer[2]) << 40) |
           (static_cast<std::uint64_t>(buffer[3]) << 32) |
           (static_cast<std::uint64_t>(buffer[4]) << 24) |
           (static_cast<std::uint64_t>(buffer[5]) << 16) |
           (static_cast<std::uint64_t>(buffer[6]) << 8) |
           static_cast<std::uint64_t>(buffer[7]);
}

Header decodeHeader(std::span<const std::byte, 24> buffer) {
    Header header;
    header.magic = static_cast<std::uint8_t>(buffer[0]);
    header.opcode = static_cast<std::uint8_t>(buffer[1]);
    header.keyLength = decodeUint16(buffer.subspan<2, 2>());
    header.extrasLength = static_cast<std::uint8_t>(buffer[4]);
    header.dataType = static_cast<std::uint8_t>(buffer[5]);
    header.vbucketOrStatus = decodeUint16(buffer.subspan<6, 2>());
    header.totalBodyLength = decodeUint32(buffer.subspan<8, 4>());
    header.opaque = decodeUint32(buffer.subspan<12, 4>());
    header.cas = decodeUint64(buffer.subspan<16, 8>());
    return header;
}

std::expected<Response, std::error_code>
parseResponse(std::span<const std::byte> buffer) {
    if (buffer.size() < 24) {
        return std::unexpected{make_error_code(Result::FAILURE)};
    }

    std::array<std::byte, 24> headerBytes;
    std::copy_n(buffer.begin(), 24, headerBytes.begin());
    Header header = decodeHeader(headerBytes);

    if (header.magic != RESPONSE_MAGIC) {
        return std::unexpected{make_error_code(Result::FAILURE)};
    }

    if (buffer.size() < 24 + header.totalBodyLength) {
        return std::unexpected{make_error_code(Result::FAILURE)};
    }

    Response response;
    response.opcode = static_cast<Opcode>(header.opcode);
    response.status = static_cast<Status>(header.vbucketOrStatus);
    response.opaque = header.opaque;
    response.cas = header.cas;
    response.flags = 0;

    std::size_t offset = 24;

    // Parse extras
    if (header.extrasLength > 0) {
        if (header.extrasLength >= 4) {
            std::array<std::byte, 4> flagsBytes;
            std::copy_n(buffer.begin() + offset, 4, flagsBytes.begin());
            response.flags = decodeUint32(flagsBytes);
        }
        offset += header.extrasLength;
    }

    // Parse key
    if (header.keyLength > 0) {
        response.key = std::string(
            reinterpret_cast<const char*>(buffer.data() + offset),
            header.keyLength
        );
        offset += header.keyLength;
    }

    // Parse value
    std::size_t valueLength = header.totalBodyLength - header.extrasLength - header.keyLength;
    if (valueLength > 0) {
        if (response.status != Status::NO_ERROR) {
            // Error message
            response.errorMessage = std::string(
                reinterpret_cast<const char*>(buffer.data() + offset),
                valueLength
            );
        } else {
            // Value data
            response.value.resize(valueLength);
            std::copy_n(buffer.begin() + offset, valueLength, response.value.begin());
        }
    }

    return response;
}

// ==================== Request Builders ====================

static std::vector<std::byte> buildRequest(
    Opcode opcode,
    std::string_view key,
    std::span<const std::byte> extras,
    std::span<const std::byte> value,
    std::uint64_t cas,
    std::uint32_t opaque
) {
    std::vector<std::byte> packet;
    packet.resize(24 + extras.size() + key.size() + value.size());

    Header header{};
    header.magic = REQUEST_MAGIC;
    header.opcode = static_cast<std::uint8_t>(opcode);
    header.keyLength = static_cast<std::uint16_t>(key.size());
    header.extrasLength = static_cast<std::uint8_t>(extras.size());
    header.dataType = 0x00;
    header.vbucketOrStatus = 0;
    header.totalBodyLength = static_cast<std::uint32_t>(extras.size() + key.size() + value.size());
    header.opaque = opaque;
    header.cas = cas;

    std::array<std::byte, 24> headerBytes;
    encodeHeader(header, headerBytes);

    std::size_t offset = 0;
    std::copy(headerBytes.begin(), headerBytes.end(), packet.begin());
    offset += 24;

    if (!extras.empty()) {
        std::copy(extras.begin(), extras.end(), packet.begin() + offset);
        offset += extras.size();
    }

    if (!key.empty()) {
        std::transform(key.begin(), key.end(), packet.begin() + offset,
                       [](char c) { return static_cast<std::byte>(c); });
        offset += key.size();
    }

    if (!value.empty()) {
        std::copy(value.begin(), value.end(), packet.begin() + offset);
    }

    return packet;
}

std::vector<std::byte> buildGetRequest(std::string_view key, std::uint32_t opaque) {
    return buildRequest(Opcode::GET, key, {}, {}, 0, opaque);
}

std::vector<std::byte> buildGetKRequest(std::string_view key, std::uint32_t opaque) {
    return buildRequest(Opcode::GETK, key, {}, {}, 0, opaque);
}

std::vector<std::byte> buildSetRequest(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint32_t flags,
    std::uint32_t expiration,
    std::uint64_t cas,
    std::uint32_t opaque
) {
    std::array<std::byte, 8> extras;
    encodeUint32(flags, std::span<std::byte, 4>(extras.data(), 4));
    encodeUint32(expiration, std::span<std::byte, 4>(extras.data() + 4, 4));
    return buildRequest(Opcode::SET, key, extras, value, cas, opaque);
}

std::vector<std::byte> buildAddRequest(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint32_t flags,
    std::uint32_t expiration,
    std::uint32_t opaque
) {
    std::array<std::byte, 8> extras;
    encodeUint32(flags, std::span<std::byte, 4>(extras.data(), 4));
    encodeUint32(expiration, std::span<std::byte, 4>(extras.data() + 4, 4));
    return buildRequest(Opcode::ADD, key, extras, value, 0, opaque);
}

std::vector<std::byte> buildReplaceRequest(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint32_t flags,
    std::uint32_t expiration,
    std::uint64_t cas,
    std::uint32_t opaque
) {
    std::array<std::byte, 8> extras;
    encodeUint32(flags, std::span<std::byte, 4>(extras.data(), 4));
    encodeUint32(expiration, std::span<std::byte, 4>(extras.data() + 4, 4));
    return buildRequest(Opcode::REPLACE, key, extras, value, cas, opaque);
}

std::vector<std::byte> buildDeleteRequest(std::string_view key, std::uint32_t opaque) {
    return buildRequest(Opcode::DELETE, key, {}, {}, 0, opaque);
}

std::vector<std::byte> buildIncrRequest(
    std::string_view key,
    std::uint64_t delta,
    std::uint64_t initial,
    std::uint32_t expiration,
    std::uint32_t opaque
) {
    std::array<std::byte, 20> extras;
    encodeUint64(delta, std::span<std::byte, 8>(extras.data(), 8));
    encodeUint64(initial, std::span<std::byte, 8>(extras.data() + 8, 8));
    encodeUint32(expiration, std::span<std::byte, 4>(extras.data() + 16, 4));
    return buildRequest(Opcode::INCREMENT, key, extras, {}, 0, opaque);
}

std::vector<std::byte> buildDecrRequest(
    std::string_view key,
    std::uint64_t delta,
    std::uint64_t initial,
    std::uint32_t expiration,
    std::uint32_t opaque
) {
    std::array<std::byte, 20> extras;
    encodeUint64(delta, std::span<std::byte, 8>(extras.data(), 8));
    encodeUint64(initial, std::span<std::byte, 8>(extras.data() + 8, 8));
    encodeUint32(expiration, std::span<std::byte, 4>(extras.data() + 16, 4));
    return buildRequest(Opcode::DECREMENT, key, extras, {}, 0, opaque);
}

std::vector<std::byte> buildAppendRequest(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint64_t cas,
    std::uint32_t opaque
) {
    return buildRequest(Opcode::APPEND, key, {}, value, cas, opaque);
}

std::vector<std::byte> buildPrependRequest(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint64_t cas,
    std::uint32_t opaque
) {
    return buildRequest(Opcode::PREPEND, key, {}, value, cas, opaque);
}

std::vector<std::byte> buildTouchRequest(
    std::string_view key,
    std::uint32_t expiration,
    std::uint32_t opaque
) {
    std::array<std::byte, 4> extras;
    encodeUint32(expiration, extras);
    return buildRequest(Opcode::TOUCH, key, extras, {}, 0, opaque);
}

std::vector<std::byte> buildGatRequest(
    std::string_view key,
    std::uint32_t expiration,
    std::uint32_t opaque
) {
    std::array<std::byte, 4> extras;
    encodeUint32(expiration, extras);
    return buildRequest(Opcode::GAT, key, extras, {}, 0, opaque);
}

std::vector<std::byte> buildFlushRequest(std::uint32_t delay, std::uint32_t opaque) {
    if (delay == 0) {
        return buildRequest(Opcode::FLUSH, "", {}, {}, 0, opaque);
    }
    std::array<std::byte, 4> extras;
    encodeUint32(delay, extras);
    return buildRequest(Opcode::FLUSH, "", extras, {}, 0, opaque);
}

std::vector<std::byte> buildStatRequest(std::string_view key, std::uint32_t opaque) {
    return buildRequest(Opcode::STAT, key, {}, {}, 0, opaque);
}

std::vector<std::byte> buildVersionRequest(std::uint32_t opaque) {
    return buildRequest(Opcode::VERSION, "", {}, {}, 0, opaque);
}

std::vector<std::byte> buildNoopRequest(std::uint32_t opaque) {
    return buildRequest(Opcode::NOOP, "", {}, {}, 0, opaque);
}

std::vector<std::byte> buildQuitRequest(std::uint32_t opaque) {
    return buildRequest(Opcode::QUIT, "", {}, {}, 0, opaque);
}

std::vector<std::byte> buildMultiGetRequest(
    std::span<const std::string> keys,
    std::uint32_t startOpaque
) {
    std::vector<std::byte> packet;

    // Use GETKQ (quiet) for all but trigger NOOP at the end
    for (std::size_t i = 0; i < keys.size(); ++i) {
        auto req = buildRequest(Opcode::GETKQ, keys[i], {}, {}, 0, startOpaque + i);
        packet.insert(packet.end(), req.begin(), req.end());
    }

    // Add NOOP to trigger responses
    auto noop = buildNoopRequest(startOpaque + static_cast<std::uint32_t>(keys.size()));
    packet.insert(packet.end(), noop.begin(), noop.end());

    return packet;
}

// ==================== Helpers ====================

std::error_code statusToError(Status status) {
    switch (status) {
        case Status::NO_ERROR:
            return {};
        case Status::KEY_NOT_FOUND:
            return make_error_code(Result::NOT_FOUND);
        case Status::KEY_EXISTS:
            return make_error_code(Result::EXISTS);
        case Status::ITEM_NOT_STORED:
            return make_error_code(Result::NOT_STORED);
        case Status::AUTHENTICATION_ERROR:
            return make_error_code(Result::CONNECTION_ERROR);
        default:
            return make_error_code(Result::SERVER_ERROR);
    }
}

std::string_view statusMessage(Status status) {
    switch (status) {
        case Status::NO_ERROR: return "No error";
        case Status::KEY_NOT_FOUND: return "Key not found";
        case Status::KEY_EXISTS: return "Key exists";
        case Status::VALUE_TOO_LARGE: return "Value too large";
        case Status::INVALID_ARGUMENTS: return "Invalid arguments";
        case Status::ITEM_NOT_STORED: return "Item not stored";
        case Status::INCR_DECR_ON_NON_NUMERIC: return "Incr/Decr on non-numeric value";
        case Status::VBUCKET_BELONGS_TO_ANOTHER: return "VBucket belongs to another server";
        case Status::AUTHENTICATION_ERROR: return "Authentication error";
        case Status::AUTHENTICATION_CONTINUE: return "Authentication continue";
        case Status::UNKNOWN_COMMAND: return "Unknown command";
        case Status::OUT_OF_MEMORY: return "Out of memory";
        case Status::NOT_SUPPORTED: return "Not supported";
        case Status::INTERNAL_ERROR: return "Internal error";
        case Status::BUSY: return "Busy";
        case Status::TEMPORARY_FAILURE: return "Temporary failure";
        default: return "Unknown error";
    }
}

bool isQuietOpcode(Opcode opcode) {
    switch (opcode) {
        case Opcode::GETQ:
        case Opcode::GETKQ:
        case Opcode::SETQ:
        case Opcode::ADDQ:
        case Opcode::REPLACEQ:
        case Opcode::DELETEQ:
        case Opcode::INCREMENTQ:
        case Opcode::DECREMENTQ:
        case Opcode::QUITQ:
        case Opcode::FLUSHQ:
        case Opcode::APPENDQ:
        case Opcode::PREPENDQ:
        case Opcode::GATQ:
        case Opcode::GATKQ:
            return true;
        default:
            return false;
    }
}

std::optional<Opcode> getQuietOpcode(Opcode opcode) {
    switch (opcode) {
        case Opcode::GET: return Opcode::GETQ;
        case Opcode::GETK: return Opcode::GETKQ;
        case Opcode::SET: return Opcode::SETQ;
        case Opcode::ADD: return Opcode::ADDQ;
        case Opcode::REPLACE: return Opcode::REPLACEQ;
        case Opcode::DELETE: return Opcode::DELETEQ;
        case Opcode::INCREMENT: return Opcode::INCREMENTQ;
        case Opcode::DECREMENT: return Opcode::DECREMENTQ;
        case Opcode::QUIT: return Opcode::QUITQ;
        case Opcode::FLUSH: return Opcode::FLUSHQ;
        case Opcode::APPEND: return Opcode::APPENDQ;
        case Opcode::PREPEND: return Opcode::PREPENDQ;
        case Opcode::GAT: return Opcode::GATQ;
        case Opcode::GATK: return Opcode::GATKQ;
        default: return std::nullopt;
    }
}

}  // namespace asyncio::memcached::protocol

namespace asyncio::memcached {

// Result error category
const std::error_category &ResultCategory::instance() {
    static ResultCategory category;
    return category;
}

std::string ResultCategory::message(int value) const {
    switch (static_cast<Result>(value)) {
        case Result::SUCCESS: return "success";
        case Result::FAILURE: return "operation failed";
        case Result::NOT_FOUND: return "key not found";
        case Result::NOT_STORED: return "value not stored";
        case Result::EXISTS: return "key already exists";
        case Result::TIMEOUT: return "operation timed out";
        case Result::CONNECTION_ERROR: return "connection error";
        case Result::SERVER_ERROR: return "server error";
        case Result::CLIENT_ERROR: return "client error";
        default: return "unknown error";
    }
}

}  // namespace asyncio::memcached
