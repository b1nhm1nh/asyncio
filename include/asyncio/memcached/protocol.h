#ifndef ASYNCIO_MEMCACHED_PROTOCOL_H
#define ASYNCIO_MEMCACHED_PROTOCOL_H

#include <asyncio/memcached/client.h>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace asyncio::memcached::protocol {

// Memcached Binary Protocol
// Reference: https://github.com/memcached/memcached/wiki/BinaryProtocolRevamped

/// Magic bytes
constexpr std::uint8_t REQUEST_MAGIC = 0x80;
constexpr std::uint8_t RESPONSE_MAGIC = 0x81;

/// Command opcodes
enum class Opcode : std::uint8_t {
    GET = 0x00,
    SET = 0x01,
    ADD = 0x02,
    REPLACE = 0x03,
    DELETE = 0x04,
    INCREMENT = 0x05,
    DECREMENT = 0x06,
    QUIT = 0x07,
    FLUSH = 0x08,
    GETQ = 0x09,       // Get quietly (no response on miss)
    NOOP = 0x0a,
    VERSION = 0x0b,
    GETK = 0x0c,       // Get with key
    GETKQ = 0x0d,      // Get with key quietly
    APPEND = 0x0e,
    PREPEND = 0x0f,
    STAT = 0x10,
    SETQ = 0x11,       // Set quietly
    ADDQ = 0x12,
    REPLACEQ = 0x13,
    DELETEQ = 0x14,
    INCREMENTQ = 0x15,
    DECREMENTQ = 0x16,
    QUITQ = 0x17,
    FLUSHQ = 0x18,
    APPENDQ = 0x19,
    PREPENDQ = 0x1a,
    TOUCH = 0x1c,
    GAT = 0x1d,        // Get and touch
    GATQ = 0x1e,
    SASL_LIST_MECHS = 0x20,
    SASL_AUTH = 0x21,
    SASL_STEP = 0x22,
    GATK = 0x23,       // Get and touch with key
    GATKQ = 0x24,
};

/// Response status codes
enum class Status : std::uint16_t {
    NO_ERROR = 0x0000,
    KEY_NOT_FOUND = 0x0001,
    KEY_EXISTS = 0x0002,
    VALUE_TOO_LARGE = 0x0003,
    INVALID_ARGUMENTS = 0x0004,
    ITEM_NOT_STORED = 0x0005,
    INCR_DECR_ON_NON_NUMERIC = 0x0006,
    VBUCKET_BELONGS_TO_ANOTHER = 0x0007,
    AUTHENTICATION_ERROR = 0x0008,
    AUTHENTICATION_CONTINUE = 0x0009,
    UNKNOWN_COMMAND = 0x0081,
    OUT_OF_MEMORY = 0x0082,
    NOT_SUPPORTED = 0x0083,
    INTERNAL_ERROR = 0x0084,
    BUSY = 0x0085,
    TEMPORARY_FAILURE = 0x0086,
};

/// Binary protocol header (24 bytes)
struct Header {
    std::uint8_t magic;
    std::uint8_t opcode;
    std::uint16_t keyLength;
    std::uint8_t extrasLength;
    std::uint8_t dataType;       // Reserved, always 0x00
    std::uint16_t vbucketOrStatus;  // Request: vbucket, Response: status
    std::uint32_t totalBodyLength;
    std::uint32_t opaque;        // Request ID, echoed in response
    std::uint64_t cas;           // CAS value
};

static_assert(sizeof(Header) == 24, "Header must be 24 bytes");

/// Request extras for SET/ADD/REPLACE
struct StorageExtras {
    std::uint32_t flags;
    std::uint32_t expiration;
};

static_assert(sizeof(StorageExtras) == 8, "StorageExtras must be 8 bytes");

/// Request extras for INCR/DECR (must be packed for wire format)
#pragma pack(push, 1)
struct CounterExtras {
    std::uint64_t delta;
    std::uint64_t initial;
    std::uint32_t expiration;
};
#pragma pack(pop)

static_assert(sizeof(CounterExtras) == 20, "CounterExtras must be 20 bytes");

/// Request extras for TOUCH/GAT
struct TouchExtras {
    std::uint32_t expiration;
};

static_assert(sizeof(TouchExtras) == 4, "TouchExtras must be 4 bytes");

/// Request extras for FLUSH
struct FlushExtras {
    std::uint32_t expiration;  // Optional delay
};

/// Parsed response
struct Response {
    Opcode opcode;
    Status status;
    std::uint32_t opaque;
    std::uint64_t cas;
    std::uint32_t flags;  // From extras if present
    std::string key;
    std::vector<std::byte> value;
    std::string errorMessage;  // For error responses
};

// ==================== Encoding Functions ====================

/// Encode header to buffer (24 bytes)
void encodeHeader(const Header &header, std::span<std::byte, 24> buffer);

/// Encode 16-bit integer (network byte order)
void encodeUint16(std::uint16_t value, std::span<std::byte, 2> buffer);

/// Encode 32-bit integer (network byte order)
void encodeUint32(std::uint32_t value, std::span<std::byte, 4> buffer);

/// Encode 64-bit integer (network byte order)
void encodeUint64(std::uint64_t value, std::span<std::byte, 8> buffer);

// ==================== Decoding Functions ====================

/// Decode header from buffer
Header decodeHeader(std::span<const std::byte, 24> buffer);

/// Decode 16-bit integer (network byte order)
std::uint16_t decodeUint16(std::span<const std::byte, 2> buffer);

/// Decode 32-bit integer (network byte order)
std::uint32_t decodeUint32(std::span<const std::byte, 4> buffer);

/// Decode 64-bit integer (network byte order)
std::uint64_t decodeUint64(std::span<const std::byte, 8> buffer);

/// Parse response from buffer (header + body)
std::expected<Response, std::error_code>
parseResponse(std::span<const std::byte> buffer);

// ==================== Request Builders ====================

/// Build GET request
std::vector<std::byte> buildGetRequest(
    std::string_view key,
    std::uint32_t opaque = 0
);

/// Build GETK request (returns key in response)
std::vector<std::byte> buildGetKRequest(
    std::string_view key,
    std::uint32_t opaque = 0
);

/// Build SET request
std::vector<std::byte> buildSetRequest(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint32_t flags = 0,
    std::uint32_t expiration = 0,
    std::uint64_t cas = 0,
    std::uint32_t opaque = 0
);

/// Build ADD request
std::vector<std::byte> buildAddRequest(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint32_t flags = 0,
    std::uint32_t expiration = 0,
    std::uint32_t opaque = 0
);

/// Build REPLACE request
std::vector<std::byte> buildReplaceRequest(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint32_t flags = 0,
    std::uint32_t expiration = 0,
    std::uint64_t cas = 0,
    std::uint32_t opaque = 0
);

/// Build DELETE request
std::vector<std::byte> buildDeleteRequest(
    std::string_view key,
    std::uint32_t opaque = 0
);

/// Build INCREMENT request
std::vector<std::byte> buildIncrRequest(
    std::string_view key,
    std::uint64_t delta = 1,
    std::uint64_t initial = 0,
    std::uint32_t expiration = 0xFFFFFFFF,  // 0xFFFFFFFF means fail if not exists
    std::uint32_t opaque = 0
);

/// Build DECREMENT request
std::vector<std::byte> buildDecrRequest(
    std::string_view key,
    std::uint64_t delta = 1,
    std::uint64_t initial = 0,
    std::uint32_t expiration = 0xFFFFFFFF,
    std::uint32_t opaque = 0
);

/// Build APPEND request
std::vector<std::byte> buildAppendRequest(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint64_t cas = 0,
    std::uint32_t opaque = 0
);

/// Build PREPEND request
std::vector<std::byte> buildPrependRequest(
    std::string_view key,
    std::span<const std::byte> value,
    std::uint64_t cas = 0,
    std::uint32_t opaque = 0
);

/// Build TOUCH request
std::vector<std::byte> buildTouchRequest(
    std::string_view key,
    std::uint32_t expiration,
    std::uint32_t opaque = 0
);

/// Build GAT (get and touch) request
std::vector<std::byte> buildGatRequest(
    std::string_view key,
    std::uint32_t expiration,
    std::uint32_t opaque = 0
);

/// Build FLUSH request
std::vector<std::byte> buildFlushRequest(
    std::uint32_t delay = 0,
    std::uint32_t opaque = 0
);

/// Build STAT request
std::vector<std::byte> buildStatRequest(
    std::string_view key = "",
    std::uint32_t opaque = 0
);

/// Build VERSION request
std::vector<std::byte> buildVersionRequest(
    std::uint32_t opaque = 0
);

/// Build NOOP request
std::vector<std::byte> buildNoopRequest(
    std::uint32_t opaque = 0
);

/// Build QUIT request
std::vector<std::byte> buildQuitRequest(
    std::uint32_t opaque = 0
);

// ==================== Multi-get Support ====================

/// Build batch GET requests (GETKQ + NOOP)
/// Uses quiet gets followed by NOOP to know when all responses received
std::vector<std::byte> buildMultiGetRequest(
    std::span<const std::string> keys,
    std::uint32_t startOpaque = 0
);

// ==================== Helpers ====================

/// Convert Status to error_code
std::error_code statusToError(Status status);

/// Get status description
std::string_view statusMessage(Status status);

/// Check if opcode is "quiet" (no response on success/miss)
bool isQuietOpcode(Opcode opcode);

/// Get quiet version of opcode (if exists)
std::optional<Opcode> getQuietOpcode(Opcode opcode);

}  // namespace asyncio::memcached::protocol

#endif // ASYNCIO_MEMCACHED_PROTOCOL_H
