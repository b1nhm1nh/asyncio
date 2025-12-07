#include <catch_extensions.h>
#include <asyncio/memcached/client.h>
#include <asyncio/time.h>

using namespace asyncio::memcached;

// Integration tests require a running memcached server
// Default: localhost:11211
// Skip with: ctest --exclude-regex "memcached.*integration"

static const char* MEMCACHED_HOST = "127.0.0.1";
static constexpr std::uint16_t MEMCACHED_PORT = 11211;

ASYNC_TEST_CASE("memcached basic get/set", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    // Set a value
    auto setResult = co_await client.set("test_key", "test_value");
    REQUIRE(setResult);

    // Get the value back
    auto getResult = co_await client.getString("test_key");
    REQUIRE(getResult);
    REQUIRE(*getResult == "test_value");

    // Clean up
    auto delResult = co_await client.del("test_key");
    REQUIRE(delResult);

    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached binary data", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    std::vector<std::byte> binaryData = {
        std::byte{0x00}, std::byte{0x01}, std::byte{0xFF},
        std::byte{0x80}, std::byte{0x7F}, std::byte{0x00}
    };

    auto setResult = co_await client.set("binary_key", binaryData);
    REQUIRE(setResult);

    auto getResult = co_await client.get("binary_key");
    REQUIRE(getResult);
    REQUIRE(getResult->has_value());
    REQUIRE((*getResult)->data == binaryData);

    co_await client.del("binary_key");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached add/replace", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    // Delete first to ensure key doesn't exist
    co_await client.del("add_test");

    // Add should succeed for new key
    auto addResult = co_await client.add("add_test", "initial");
    REQUIRE(addResult);
    REQUIRE(*addResult == true);

    // Add should fail for existing key
    auto addResult2 = co_await client.add("add_test", "duplicate");
    REQUIRE(addResult2);
    REQUIRE(*addResult2 == false);

    // Replace should succeed for existing key
    auto replaceResult = co_await client.replace("add_test", "replaced");
    REQUIRE(replaceResult);
    REQUIRE(*replaceResult == true);

    // Verify replacement
    auto getResult = co_await client.getString("add_test");
    REQUIRE(getResult);
    REQUIRE(*getResult == "replaced");

    // Replace should fail for non-existent key
    co_await client.del("nonexistent_key");
    auto replaceResult2 = co_await client.replace("nonexistent_key", "value");
    REQUIRE(replaceResult2);
    REQUIRE(*replaceResult2 == false);

    co_await client.del("add_test");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached increment/decrement", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    // Set initial numeric value
    co_await client.set("counter", "100");

    // Increment by 1
    auto incrResult = co_await client.incr("counter", 1);
    REQUIRE(incrResult);
    REQUIRE(*incrResult == 101);

    // Increment by 10
    auto incrResult2 = co_await client.incr("counter", 10);
    REQUIRE(incrResult2);
    REQUIRE(*incrResult2 == 111);

    // Decrement by 5
    auto decrResult = co_await client.decr("counter", 5);
    REQUIRE(decrResult);
    REQUIRE(*decrResult == 106);

    co_await client.del("counter");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached append/prepend", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    // Set initial value
    co_await client.set("concat_test", "Hello");

    // Append
    auto appendResult = co_await client.append("concat_test", " World");
    REQUIRE(appendResult);

    auto getValue = co_await client.getString("concat_test");
    REQUIRE(getValue);
    REQUIRE(*getValue == "Hello World");

    // Prepend
    auto prependResult = co_await client.prepend("concat_test", "Say: ");
    REQUIRE(prependResult);

    auto getValue2 = co_await client.getString("concat_test");
    REQUIRE(getValue2);
    REQUIRE(*getValue2 == "Say: Hello World");

    co_await client.del("concat_test");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached key not found", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    // Try to get non-existent key
    auto getResult = co_await client.get("definitely_not_existing_key_12345");
    REQUIRE(getResult);
    REQUIRE(!getResult->has_value());

    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached delete", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    // Set and delete
    co_await client.set("delete_test", "value");

    auto delResult = co_await client.del("delete_test");
    REQUIRE(delResult);
    REQUIRE(*delResult == true);

    // Verify it's gone
    auto getResult = co_await client.get("delete_test");
    REQUIRE(getResult);
    REQUIRE(!getResult->has_value());

    // Delete non-existent key
    auto delResult2 = co_await client.del("nonexistent_delete_key");
    REQUIRE(delResult2);
    REQUIRE(*delResult2 == false);

    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached touch", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    // Set with expiration
    co_await client.set("touch_test", "value", std::chrono::seconds{60});

    // Touch to extend expiration
    auto touchResult = co_await client.touch("touch_test", std::chrono::seconds{120});
    REQUIRE(touchResult);
    REQUIRE(*touchResult == true);

    // Touch non-existent key
    auto touchResult2 = co_await client.touch("nonexistent_touch", std::chrono::seconds{60});
    REQUIRE(touchResult2);
    REQUIRE(*touchResult2 == false);

    co_await client.del("touch_test");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached gat (get and touch)", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    // Set value
    co_await client.set("gat_test", "gat_value");

    // Get and touch
    auto gatResult = co_await client.gat("gat_test", std::chrono::seconds{120});
    REQUIRE(gatResult);
    REQUIRE(gatResult->has_value());
    REQUIRE((*gatResult)->asString() == "gat_value");

    co_await client.del("gat_test");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached version", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    auto versionResult = co_await client.version();
    REQUIRE(versionResult);
    REQUIRE(!versionResult->empty());

    // Version should contain version string
    for (const auto& [server, version] : *versionResult) {
        REQUIRE(!version.empty());
    }

    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached stats", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    auto statsResult = co_await client.stats();
    REQUIRE(statsResult);
    REQUIRE(!statsResult->empty());

    // Should have some stats
    for (const auto& serverStats : *statsResult) {
        REQUIRE(!serverStats.stats.empty());
    }

    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached expiration", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    // Set with 1 second expiration
    co_await client.set("expire_test", "value", std::chrono::seconds{1});

    // Should exist immediately
    auto getResult1 = co_await client.getString("expire_test");
    REQUIRE(getResult1);
    REQUIRE(*getResult1 == "value");

    // Wait for expiration
    co_await asyncio::sleep(std::chrono::seconds{2});

    // Should be gone
    auto getResult2 = co_await client.get("expire_test");
    REQUIRE(getResult2);
    REQUIRE(!getResult2->has_value());

    co_await client.disconnect();
}

ASYNC_TEST_CASE("memcached flags", "[memcached][integration]") {
    auto clientResult = co_await Client::connect(MEMCACHED_HOST, MEMCACHED_PORT);
    if (!clientResult) {
        SKIP("Memcached server not available");
        co_return;
    }
    auto& client = *clientResult;

    // Set with custom flags
    std::uint32_t customFlags = 0x12345678;
    co_await client.set("flags_test", "value", std::chrono::seconds{0}, customFlags);

    auto getResult = co_await client.get("flags_test");
    REQUIRE(getResult);
    REQUIRE(getResult->has_value());
    REQUIRE((*getResult)->flags == customFlags);

    co_await client.del("flags_test");
    co_await client.disconnect();
}
