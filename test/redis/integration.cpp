#include <catch_extensions.h>
#include <asyncio/redis/client.h>
#include <asyncio/time.h>

using namespace asyncio::redis;

// Integration tests require a running Redis server
// Default: localhost:6379
// Skip with: ctest --exclude-regex "redis.*integration"

static const char* REDIS_HOST = "127.0.0.1";
static constexpr std::uint16_t REDIS_PORT = 6379;

// Helper macro to connect or skip
#define CONNECT_OR_SKIP()                                       \
    Config config;                                              \
    config.host = REDIS_HOST;                                   \
    config.port = REDIS_PORT;                                   \
    config.connectTimeout = std::chrono::milliseconds{1000};    \
    auto clientResult = co_await Client::connect(config);       \
    if (!clientResult) {                                        \
        SKIP("Redis server not available");                     \
        co_return;                                              \
    }                                                           \
    auto& client = *clientResult

ASYNC_TEST_CASE("redis ping", "[redis][integration]") {
    CONNECT_OR_SKIP();

    auto pingResult = co_await client.ping();
    REQUIRE(pingResult);
    REQUIRE(*pingResult == "PONG");

    auto pingWithMsg = co_await client.ping("hello");
    REQUIRE(pingWithMsg);
    REQUIRE(*pingWithMsg == "hello");

    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis echo", "[redis][integration]") {
    CONNECT_OR_SKIP();

    auto echoResult = co_await client.echo("Hello Redis!");
    REQUIRE(echoResult);
    REQUIRE(*echoResult == "Hello Redis!");

    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis basic get/set", "[redis][integration]") {
    CONNECT_OR_SKIP();

    // Set a value
    auto setResult = co_await client.set("test_key", "test_value");
    REQUIRE(setResult);
    REQUIRE(*setResult == true);

    // Get the value back
    auto getResult = co_await client.get("test_key");
    REQUIRE(getResult);
    REQUIRE(*getResult == "test_value");

    // Clean up
    auto delResult = co_await client.del("test_key");
    REQUIRE(delResult);
    REQUIRE(*delResult == true);

    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis set with TTL", "[redis][integration]") {
    CONNECT_OR_SKIP();

    // Set with TTL
    auto setResult = co_await client.set("ttl_key", "value", std::chrono::milliseconds{2000});
    REQUIRE(setResult);

    // Check TTL
    auto ttlResult = co_await client.pttl("ttl_key");
    REQUIRE(ttlResult);
    REQUIRE(*ttlResult > 0);
    REQUIRE(*ttlResult <= 2000);

    co_await client.del("ttl_key");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis set NX/XX", "[redis][integration]") {
    CONNECT_OR_SKIP();

    // Clean up first
    co_await client.del("nx_test");

    // NX should succeed on new key
    auto nxResult1 = co_await client.set("nx_test", "value1", std::chrono::milliseconds{0}, true, false);
    REQUIRE(nxResult1);
    REQUIRE(*nxResult1 == true);

    // NX should fail on existing key
    auto nxResult2 = co_await client.set("nx_test", "value2", std::chrono::milliseconds{0}, true, false);
    REQUIRE(nxResult2);
    REQUIRE(*nxResult2 == false);

    // XX should succeed on existing key
    auto xxResult1 = co_await client.set("nx_test", "value3", std::chrono::milliseconds{0}, false, true);
    REQUIRE(xxResult1);
    REQUIRE(*xxResult1 == true);

    // Verify value was updated
    auto getResult = co_await client.get("nx_test");
    REQUIRE(getResult);
    REQUIRE(*getResult == "value3");

    // XX should fail on non-existent key
    co_await client.del("xx_test");
    auto xxResult2 = co_await client.set("xx_test", "value", std::chrono::milliseconds{0}, false, true);
    REQUIRE(xxResult2);
    REQUIRE(*xxResult2 == false);

    co_await client.del("nx_test");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis setnx", "[redis][integration]") {
    CONNECT_OR_SKIP();

    co_await client.del("setnx_key");

    auto result1 = co_await client.setnx("setnx_key", "value1");
    REQUIRE(result1);
    REQUIRE(*result1 == true);

    auto result2 = co_await client.setnx("setnx_key", "value2");
    REQUIRE(result2);
    REQUIRE(*result2 == false);

    co_await client.del("setnx_key");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis mget/mset", "[redis][integration]") {
    CONNECT_OR_SKIP();

    // MSET
    std::vector<std::pair<std::string, std::string>> kvs = {
        {"mtest1", "value1"},
        {"mtest2", "value2"},
        {"mtest3", "value3"}
    };
    auto msetResult = co_await client.mset(kvs);
    REQUIRE(msetResult);

    // MGET
    std::vector<std::string> keys = {"mtest1", "mtest2", "mtest3", "nonexistent"};
    auto mgetResult = co_await client.mget(keys);
    REQUIRE(mgetResult);
    REQUIRE(mgetResult->size() == 4);
    REQUIRE((*mgetResult)[0] == "value1");
    REQUIRE((*mgetResult)[1] == "value2");
    REQUIRE((*mgetResult)[2] == "value3");
    REQUIRE(!(*mgetResult)[3].has_value());  // nonexistent key

    co_await client.del(std::vector<std::string>{"mtest1", "mtest2", "mtest3"});
    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis incr/decr", "[redis][integration]") {
    CONNECT_OR_SKIP();

    co_await client.del("counter");
    co_await client.set("counter", "10");

    auto incrResult = co_await client.incr("counter");
    REQUIRE(incrResult);
    REQUIRE(*incrResult == 11);

    auto incrbyResult = co_await client.incrby("counter", 5);
    REQUIRE(incrbyResult);
    REQUIRE(*incrbyResult == 16);

    auto decrResult = co_await client.decr("counter");
    REQUIRE(decrResult);
    REQUIRE(*decrResult == 15);

    co_await client.del("counter");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis key operations", "[redis][integration]") {
    CONNECT_OR_SKIP();

    co_await client.set("key_test", "value");

    // EXISTS
    auto existsResult = co_await client.exists("key_test");
    REQUIRE(existsResult);
    REQUIRE(*existsResult == true);

    auto notExistsResult = co_await client.exists("nonexistent_key_12345");
    REQUIRE(notExistsResult);
    REQUIRE(*notExistsResult == false);

    // TYPE
    auto typeResult = co_await client.type("key_test");
    REQUIRE(typeResult);
    REQUIRE(*typeResult == "string");

    // EXPIRE and TTL
    auto expireResult = co_await client.expire("key_test", std::chrono::seconds{60});
    REQUIRE(expireResult);
    REQUIRE(*expireResult == true);

    auto ttlResult = co_await client.ttl("key_test");
    REQUIRE(ttlResult);
    REQUIRE(*ttlResult > 0);
    REQUIRE(*ttlResult <= 60);

    // RENAME
    auto renameResult = co_await client.rename("key_test", "key_test_renamed");
    REQUIRE(renameResult);

    auto getRenamed = co_await client.get("key_test_renamed");
    REQUIRE(getRenamed);
    REQUIRE(*getRenamed == "value");

    co_await client.del("key_test_renamed");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis hash operations", "[redis][integration]") {
    CONNECT_OR_SKIP();

    co_await client.del("hash_test");

    // HSET
    auto hsetResult = co_await client.hset("hash_test", "field1", "value1");
    REQUIRE(hsetResult);
    REQUIRE(*hsetResult == true);  // New field

    auto hsetResult2 = co_await client.hset("hash_test", "field1", "value1_updated");
    REQUIRE(hsetResult2);
    REQUIRE(*hsetResult2 == false);  // Existing field

    // HGET
    auto hgetResult = co_await client.hget("hash_test", "field1");
    REQUIRE(hgetResult);
    REQUIRE(*hgetResult == "value1_updated");

    // HEXISTS
    auto hexistsResult = co_await client.hexists("hash_test", "field1");
    REQUIRE(hexistsResult);
    REQUIRE(*hexistsResult == true);

    auto hexistsResult2 = co_await client.hexists("hash_test", "nonexistent");
    REQUIRE(hexistsResult2);
    REQUIRE(*hexistsResult2 == false);

    // HLEN
    co_await client.hset("hash_test", "field2", "value2");
    auto hlenResult = co_await client.hlen("hash_test");
    REQUIRE(hlenResult);
    REQUIRE(*hlenResult == 2);

    // HGETALL
    auto hgetallResult = co_await client.hgetall("hash_test");
    REQUIRE(hgetallResult);
    REQUIRE(hgetallResult->size() == 2);

    // HINCRBY
    co_await client.hset("hash_test", "counter", "10");
    auto hincrResult = co_await client.hincrby("hash_test", "counter", 5);
    REQUIRE(hincrResult);
    REQUIRE(*hincrResult == 15);

    // HDEL
    auto hdelResult = co_await client.hdel("hash_test", std::vector<std::string>{"field1", "field2"});
    REQUIRE(hdelResult);
    REQUIRE(*hdelResult == 2);

    co_await client.del("hash_test");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis list operations", "[redis][integration]") {
    CONNECT_OR_SKIP();

    co_await client.del("list_test");

    // LPUSH
    auto lpushResult = co_await client.lpush("list_test", std::vector<std::string>{"a", "b", "c"});
    REQUIRE(lpushResult);
    REQUIRE(*lpushResult == 3);

    // RPUSH
    auto rpushResult = co_await client.rpush("list_test", std::vector<std::string>{"d", "e"});
    REQUIRE(rpushResult);
    REQUIRE(*rpushResult == 5);

    // LLEN
    auto llenResult = co_await client.llen("list_test");
    REQUIRE(llenResult);
    REQUIRE(*llenResult == 5);

    // LRANGE
    auto lrangeResult = co_await client.lrange("list_test", 0, -1);
    REQUIRE(lrangeResult);
    REQUIRE(lrangeResult->size() == 5);
    // Order: c, b, a, d, e (lpush reverses, rpush appends)
    REQUIRE((*lrangeResult)[0] == "c");
    REQUIRE((*lrangeResult)[4] == "e");

    // LPOP
    auto lpopResult = co_await client.lpop("list_test");
    REQUIRE(lpopResult);
    REQUIRE(*lpopResult == "c");

    // RPOP
    auto rpopResult = co_await client.rpop("list_test");
    REQUIRE(rpopResult);
    REQUIRE(*rpopResult == "e");

    co_await client.del("list_test");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis set operations", "[redis][integration]") {
    CONNECT_OR_SKIP();

    co_await client.del("set_test");

    // SADD
    auto saddResult = co_await client.sadd("set_test", std::vector<std::string>{"a", "b", "c"});
    REQUIRE(saddResult);
    REQUIRE(*saddResult == 3);

    // SADD duplicates
    auto saddResult2 = co_await client.sadd("set_test", std::vector<std::string>{"a", "d"});
    REQUIRE(saddResult2);
    REQUIRE(*saddResult2 == 1);  // Only "d" is new

    // SCARD
    auto scardResult = co_await client.scard("set_test");
    REQUIRE(scardResult);
    REQUIRE(*scardResult == 4);

    // SISMEMBER
    auto sismemberResult = co_await client.sismember("set_test", "a");
    REQUIRE(sismemberResult);
    REQUIRE(*sismemberResult == true);

    auto sismemberResult2 = co_await client.sismember("set_test", "z");
    REQUIRE(sismemberResult2);
    REQUIRE(*sismemberResult2 == false);

    // SMEMBERS
    auto smembersResult = co_await client.smembers("set_test");
    REQUIRE(smembersResult);
    REQUIRE(smembersResult->size() == 4);

    // SREM
    auto sremResult = co_await client.srem("set_test", std::vector<std::string>{"a", "b"});
    REQUIRE(sremResult);
    REQUIRE(*sremResult == 2);

    co_await client.del("set_test");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis sorted set operations", "[redis][integration]") {
    CONNECT_OR_SKIP();

    co_await client.del("zset_test");

    // ZADD
    std::vector<std::pair<double, std::string>> members = {
        {1.0, "one"},
        {2.0, "two"},
        {3.0, "three"}
    };
    auto zaddResult = co_await client.zadd("zset_test", members);
    REQUIRE(zaddResult);
    REQUIRE(*zaddResult == 3);

    // ZCARD
    auto zcardResult = co_await client.zcard("zset_test");
    REQUIRE(zcardResult);
    REQUIRE(*zcardResult == 3);

    // ZSCORE
    auto zscoreResult = co_await client.zscore("zset_test", "two");
    REQUIRE(zscoreResult);
    REQUIRE(*zscoreResult == 2.0);

    // ZRANK
    auto zrankResult = co_await client.zrank("zset_test", "two");
    REQUIRE(zrankResult);
    REQUIRE(*zrankResult == 1);  // 0-indexed

    // ZRANGE
    auto zrangeResult = co_await client.zrange("zset_test", 0, -1);
    REQUIRE(zrangeResult);
    REQUIRE(zrangeResult->size() == 3);
    REQUIRE((*zrangeResult)[0] == "one");
    REQUIRE((*zrangeResult)[1] == "two");
    REQUIRE((*zrangeResult)[2] == "three");

    // ZRANGE WITHSCORES
    auto zrangeWithScores = co_await client.zrangeWithScores("zset_test", 0, -1);
    REQUIRE(zrangeWithScores);
    REQUIRE(zrangeWithScores->size() == 3);
    REQUIRE((*zrangeWithScores)[0].second == 1.0);
    REQUIRE((*zrangeWithScores)[1].second == 2.0);

    // ZREM
    auto zremResult = co_await client.zrem("zset_test", std::vector<std::string>{"one"});
    REQUIRE(zremResult);
    REQUIRE(*zremResult == 1);

    co_await client.del("zset_test");
    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis scan", "[redis][integration]") {
    CONNECT_OR_SKIP();

    // Create some test keys
    for (int i = 0; i < 10; i++) {
        co_await client.set("scan_test_" + std::to_string(i), "value");
    }

    // Scan for keys
    std::vector<std::string> allKeys;
    std::string cursor = "0";

    do {
        auto scanResult = co_await client.scan(cursor, "scan_test_*", 5);
        REQUIRE(scanResult);
        cursor = scanResult->cursor;
        for (const auto& key : scanResult->keys) {
            allKeys.push_back(key);
        }
    } while (cursor != "0");

    REQUIRE(allKeys.size() == 10);

    // Clean up
    for (int i = 0; i < 10; i++) {
        co_await client.del("scan_test_" + std::to_string(i));
    }

    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis info", "[redis][integration]") {
    CONNECT_OR_SKIP();

    auto infoResult = co_await client.info();
    REQUIRE(infoResult);
    REQUIRE(!infoResult->empty());
    REQUIRE(infoResult->find("redis_version") != std::string::npos);

    auto infoServer = co_await client.info("server");
    REQUIRE(infoServer);
    REQUIRE(!infoServer->empty());

    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis dbsize", "[redis][integration]") {
    CONNECT_OR_SKIP();

    auto dbsizeResult = co_await client.dbsize();
    REQUIRE(dbsizeResult);
    REQUIRE(*dbsizeResult >= 0);

    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis publish", "[redis][integration]") {
    CONNECT_OR_SKIP();

    // Publish to a channel (even if no subscribers)
    auto publishResult = co_await client.publish("test_channel", "test_message");
    REQUIRE(publishResult);
    // Returns number of subscribers who received the message
    REQUIRE(*publishResult >= 0);

    co_await client.disconnect();
}

ASYNC_TEST_CASE("redis URL connection", "[redis][integration]") {
    auto clientResult = co_await Client::connect("redis://127.0.0.1:6379/0");
    if (!clientResult) {
        SKIP("Redis server not available");
        co_return;
    }
    auto& client = *clientResult;

    auto pingResult = co_await client.ping();
    REQUIRE(pingResult);
    REQUIRE(*pingResult == "PONG");

    co_await client.disconnect();
}
