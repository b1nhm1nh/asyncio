#include <catch_extensions.h>
#include <asyncio/memcached/protocol.h>

using namespace asyncio::memcached;
using namespace asyncio::memcached::protocol;

TEST_CASE("Header encoding/decoding", "[memcached::protocol]") {
    SECTION("basic header") {
        Header header{};
        header.magic = REQUEST_MAGIC;
        header.opcode = static_cast<std::uint8_t>(Opcode::GET);
        header.keyLength = 5;
        header.extrasLength = 0;
        header.dataType = 0;
        header.vbucketOrStatus = 0;
        header.totalBodyLength = 5;
        header.opaque = 1234;
        header.cas = 0;

        std::array<std::byte, 24> buffer{};
        encodeHeader(header, buffer);

        auto decoded = decodeHeader(buffer);
        REQUIRE(decoded.magic == REQUEST_MAGIC);
        REQUIRE(decoded.opcode == static_cast<std::uint8_t>(Opcode::GET));
        REQUIRE(decoded.keyLength == 5);
        REQUIRE(decoded.extrasLength == 0);
        REQUIRE(decoded.totalBodyLength == 5);
        REQUIRE(decoded.opaque == 1234);
    }

    SECTION("response header with CAS") {
        Header header{};
        header.magic = RESPONSE_MAGIC;
        header.opcode = static_cast<std::uint8_t>(Opcode::SET);
        header.keyLength = 0;
        header.extrasLength = 4;
        header.dataType = 0;
        header.vbucketOrStatus = static_cast<std::uint16_t>(Status::NO_ERROR);
        header.totalBodyLength = 4;
        header.opaque = 5678;
        header.cas = 123456789012345ULL;

        std::array<std::byte, 24> buffer{};
        encodeHeader(header, buffer);

        auto decoded = decodeHeader(buffer);
        REQUIRE(decoded.magic == RESPONSE_MAGIC);
        REQUIRE(decoded.vbucketOrStatus == static_cast<std::uint16_t>(Status::NO_ERROR));
        REQUIRE(decoded.cas == 123456789012345ULL);
    }
}

TEST_CASE("Integer encoding/decoding", "[memcached::protocol]") {
    SECTION("uint16") {
        std::array<std::byte, 2> buf{};

        encodeUint16(0, buf);
        REQUIRE(decodeUint16(buf) == 0);

        encodeUint16(65535, buf);
        REQUIRE(decodeUint16(buf) == 65535);

        encodeUint16(0x1234, buf);
        REQUIRE(decodeUint16(buf) == 0x1234);
    }

    SECTION("uint32") {
        std::array<std::byte, 4> buf{};

        encodeUint32(0, buf);
        REQUIRE(decodeUint32(buf) == 0);

        encodeUint32(0xDEADBEEF, buf);
        REQUIRE(decodeUint32(buf) == 0xDEADBEEF);
    }

    SECTION("uint64") {
        std::array<std::byte, 8> buf{};

        encodeUint64(0, buf);
        REQUIRE(decodeUint64(buf) == 0);

        encodeUint64(0xDEADBEEFCAFEBABEULL, buf);
        REQUIRE(decodeUint64(buf) == 0xDEADBEEFCAFEBABEULL);
    }
}

TEST_CASE("GET request building", "[memcached::protocol]") {
    auto request = buildGetRequest("test_key", 42);

    REQUIRE(request.size() >= 24);  // At least header size

    // Verify header
    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.magic == REQUEST_MAGIC);
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::GET));
    REQUIRE(header.keyLength == 8);  // "test_key"
    REQUIRE(header.extrasLength == 0);
    REQUIRE(header.totalBodyLength == 8);
    REQUIRE(header.opaque == 42);
}

TEST_CASE("GETK request building", "[memcached::protocol]") {
    auto request = buildGetKRequest("mykey");

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::GETK));
    REQUIRE(header.keyLength == 5);
}

TEST_CASE("SET request building", "[memcached::protocol]") {
    std::vector<std::byte> value = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

    auto request = buildSetRequest("key", value, 0x12345678, 3600, 0, 100);

    REQUIRE(request.size() >= 24);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.magic == REQUEST_MAGIC);
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::SET));
    REQUIRE(header.keyLength == 3);  // "key"
    REQUIRE(header.extrasLength == 8);  // StorageExtras
    REQUIRE(header.totalBodyLength == 3 + 8 + 3);  // key + extras + value
    REQUIRE(header.opaque == 100);
}

TEST_CASE("ADD request building", "[memcached::protocol]") {
    std::vector<std::byte> value = {std::byte{0xFF}};

    auto request = buildAddRequest("newkey", value, 0, 0, 1);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::ADD));
    REQUIRE(header.keyLength == 6);
}

TEST_CASE("REPLACE request building", "[memcached::protocol]") {
    std::vector<std::byte> value = {std::byte{0xAB}};

    auto request = buildReplaceRequest("existing", value, 123, 7200, 999, 2);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::REPLACE));
    REQUIRE(header.cas == 999);
}

TEST_CASE("DELETE request building", "[memcached::protocol]") {
    auto request = buildDeleteRequest("to_delete", 123);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.magic == REQUEST_MAGIC);
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::DELETE));
    REQUIRE(header.keyLength == 9);
    REQUIRE(header.extrasLength == 0);
    REQUIRE(header.opaque == 123);
}

TEST_CASE("INCREMENT request building", "[memcached::protocol]") {
    auto request = buildIncrRequest("counter", 5, 100, 0xFFFFFFFF, 10);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::INCREMENT));
    REQUIRE(header.extrasLength == 20);  // CounterExtras
    REQUIRE(header.keyLength == 7);
}

TEST_CASE("DECREMENT request building", "[memcached::protocol]") {
    auto request = buildDecrRequest("counter", 1, 0, 3600, 11);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::DECREMENT));
    REQUIRE(header.extrasLength == 20);
}

TEST_CASE("APPEND request building", "[memcached::protocol]") {
    std::vector<std::byte> value = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};

    auto request = buildAppendRequest("key", value, 0, 5);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::APPEND));
    REQUIRE(header.extrasLength == 0);  // No extras for APPEND
}

TEST_CASE("PREPEND request building", "[memcached::protocol]") {
    std::vector<std::byte> value = {std::byte{'x'}};

    auto request = buildPrependRequest("key", value, 0, 6);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::PREPEND));
}

TEST_CASE("TOUCH request building", "[memcached::protocol]") {
    auto request = buildTouchRequest("mykey", 3600, 7);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::TOUCH));
    REQUIRE(header.extrasLength == 4);  // TouchExtras
    REQUIRE(header.keyLength == 5);
}

TEST_CASE("GAT request building", "[memcached::protocol]") {
    auto request = buildGatRequest("mykey", 7200, 8);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::GAT));
    REQUIRE(header.extrasLength == 4);
}

TEST_CASE("FLUSH request building", "[memcached::protocol]") {
    SECTION("immediate flush") {
        auto request = buildFlushRequest(0, 9);

        auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
        REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::FLUSH));
    }

    SECTION("delayed flush") {
        auto request = buildFlushRequest(30, 10);

        auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
        REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::FLUSH));
        REQUIRE(header.extrasLength == 4);
    }
}

TEST_CASE("STAT request building", "[memcached::protocol]") {
    SECTION("all stats") {
        auto request = buildStatRequest("", 11);

        auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
        REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::STAT));
        REQUIRE(header.keyLength == 0);
    }

    SECTION("specific stat") {
        auto request = buildStatRequest("items", 12);

        auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
        REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::STAT));
        REQUIRE(header.keyLength == 5);
    }
}

TEST_CASE("VERSION request building", "[memcached::protocol]") {
    auto request = buildVersionRequest(13);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::VERSION));
    REQUIRE(header.keyLength == 0);
    REQUIRE(header.extrasLength == 0);
}

TEST_CASE("NOOP request building", "[memcached::protocol]") {
    auto request = buildNoopRequest(14);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::NOOP));
    REQUIRE(header.totalBodyLength == 0);
}

TEST_CASE("QUIT request building", "[memcached::protocol]") {
    auto request = buildQuitRequest(15);

    auto header = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header.opcode == static_cast<std::uint8_t>(Opcode::QUIT));
}

TEST_CASE("Multi-get request building", "[memcached::protocol]") {
    std::vector<std::string> keys = {"key1", "key2", "key3"};

    auto request = buildMultiGetRequest(keys, 100);

    REQUIRE(!request.empty());
    // Should contain GETKQ requests followed by NOOP
    // First request should be GETKQ with opaque 100
    auto header1 = decodeHeader(std::span<const std::byte, 24>(request.data(), 24));
    REQUIRE(header1.opcode == static_cast<std::uint8_t>(Opcode::GETKQ));
    REQUIRE(header1.opaque == 100);
}

TEST_CASE("Response parsing", "[memcached::protocol]") {
    SECTION("successful GET response") {
        // Build a mock GET response
        std::vector<std::byte> response(24 + 4 + 5);  // Header + flags + value "hello"

        Header header{};
        header.magic = RESPONSE_MAGIC;
        header.opcode = static_cast<std::uint8_t>(Opcode::GET);
        header.keyLength = 0;
        header.extrasLength = 4;
        header.dataType = 0;
        header.vbucketOrStatus = 0;
        header.totalBodyLength = 9;  // 4 flags + 5 value
        header.opaque = 42;
        header.cas = 12345;

        std::array<std::byte, 24> headerBuf;
        encodeHeader(header, headerBuf);
        std::copy(headerBuf.begin(), headerBuf.end(), response.begin());

        // Extras (flags)
        response[24] = std::byte{0x00};
        response[25] = std::byte{0x00};
        response[26] = std::byte{0x00};
        response[27] = std::byte{0x01};

        // Value "hello"
        response[28] = std::byte{'h'};
        response[29] = std::byte{'e'};
        response[30] = std::byte{'l'};
        response[31] = std::byte{'l'};
        response[32] = std::byte{'o'};

        auto result = parseResponse(response);
        REQUIRE(result);
        REQUIRE(result->status == Status::NO_ERROR);
        REQUIRE(result->opaque == 42);
        REQUIRE(result->cas == 12345);
        REQUIRE(result->flags == 1);
        REQUIRE(result->value.size() == 5);
    }

    SECTION("key not found response") {
        std::vector<std::byte> response(24 + 9);  // Header + "Not found"

        Header header{};
        header.magic = RESPONSE_MAGIC;
        header.opcode = static_cast<std::uint8_t>(Opcode::GET);
        header.keyLength = 0;
        header.extrasLength = 0;
        header.dataType = 0;
        header.vbucketOrStatus = static_cast<std::uint16_t>(Status::KEY_NOT_FOUND);
        header.totalBodyLength = 9;
        header.opaque = 99;
        header.cas = 0;

        std::array<std::byte, 24> headerBuf;
        encodeHeader(header, headerBuf);
        std::copy(headerBuf.begin(), headerBuf.end(), response.begin());

        // Error message "Not found"
        const char* msg = "Not found";
        for (int i = 0; i < 9; i++) {
            response[24 + i] = static_cast<std::byte>(msg[i]);
        }

        auto result = parseResponse(response);
        REQUIRE(result);
        REQUIRE(result->status == Status::KEY_NOT_FOUND);
        REQUIRE(result->errorMessage == "Not found");
    }
}

TEST_CASE("Status to error code conversion", "[memcached::protocol]") {
    REQUIRE(statusToError(Status::NO_ERROR) == std::error_code{});
    REQUIRE(statusToError(Status::KEY_NOT_FOUND));
    REQUIRE(statusToError(Status::KEY_EXISTS));
    REQUIRE(statusToError(Status::VALUE_TOO_LARGE));
}

TEST_CASE("Status message", "[memcached::protocol]") {
    REQUIRE(!statusMessage(Status::NO_ERROR).empty());
    REQUIRE(!statusMessage(Status::KEY_NOT_FOUND).empty());
    REQUIRE(!statusMessage(Status::AUTHENTICATION_ERROR).empty());
}

TEST_CASE("Quiet opcode detection", "[memcached::protocol]") {
    REQUIRE_FALSE(isQuietOpcode(Opcode::GET));
    REQUIRE(isQuietOpcode(Opcode::GETQ));
    REQUIRE(isQuietOpcode(Opcode::GETKQ));
    REQUIRE_FALSE(isQuietOpcode(Opcode::SET));
    REQUIRE(isQuietOpcode(Opcode::SETQ));
}

TEST_CASE("Get quiet opcode", "[memcached::protocol]") {
    auto quietGet = getQuietOpcode(Opcode::GET);
    REQUIRE(quietGet);
    REQUIRE(*quietGet == Opcode::GETQ);

    auto quietSet = getQuietOpcode(Opcode::SET);
    REQUIRE(quietSet);
    REQUIRE(*quietSet == Opcode::SETQ);

    auto quietGetK = getQuietOpcode(Opcode::GETK);
    REQUIRE(quietGetK);
    REQUIRE(*quietGetK == Opcode::GETKQ);

    // NOOP doesn't have a quiet version
    auto quietNoop = getQuietOpcode(Opcode::NOOP);
    REQUIRE_FALSE(quietNoop);
}
