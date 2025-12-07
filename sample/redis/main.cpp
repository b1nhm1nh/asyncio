#include <asyncio/redis/client.h>
#include <asyncio/time.h>
#include <iostream>
#include <iomanip>

using namespace asyncio;
using namespace asyncio::redis;

task::Task<void, std::error_code> asyncMain(int argc, char *argv[]) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<std::uint16_t>(std::stoi(argv[2]));

    std::cout << "Connecting to Redis at " << host << ":" << port << "...\n";

    Config config;
    config.host = host;
    config.port = port;

    auto clientResult = co_await Client::connect(config);
    if (!clientResult) {
        std::cerr << "Failed to connect: " << clientResult.error().message() << "\n";
        co_return std::unexpected(clientResult.error());
    }

    auto& client = *clientResult;
    std::cout << "Connected!\n\n";

    // PING
    std::cout << "=== PING ===\n";
    auto pingResult = co_await client.ping();
    if (pingResult) {
        std::cout << "PING: " << *pingResult << "\n\n";
    }

    // String operations
    std::cout << "=== String Operations ===\n";

    CO_EXPECT(co_await client.set("greeting", "Hello, Redis!"));
    std::cout << "SET greeting 'Hello, Redis!'\n";

    auto getValue = co_await client.get("greeting");
    if (getValue && *getValue) {
        std::cout << "GET greeting: " << **getValue << "\n";
    }

    // With TTL
    CO_EXPECT(co_await client.set("temp_key", "expires soon", std::chrono::milliseconds{5000}));
    std::cout << "SET temp_key 'expires soon' PX 5000\n";

    auto ttl = co_await client.pttl("temp_key");
    if (ttl) {
        std::cout << "PTTL temp_key: " << *ttl << " ms\n\n";
    }

    // Counter operations
    std::cout << "=== Counter Operations ===\n";

    co_await client.set("counter", "0");
    std::cout << "SET counter 0\n";

    auto incr1 = co_await client.incr("counter");
    if (incr1) std::cout << "INCR counter: " << *incr1 << "\n";

    auto incr5 = co_await client.incrby("counter", 5);
    if (incr5) std::cout << "INCRBY counter 5: " << *incr5 << "\n";

    auto decr2 = co_await client.incrby("counter", -2);
    if (decr2) std::cout << "INCRBY counter -2: " << *decr2 << "\n\n";

    // Hash operations
    std::cout << "=== Hash Operations ===\n";

    co_await client.del("user:1000");
    CO_EXPECT(co_await client.hset("user:1000", "name", "John Doe"));
    CO_EXPECT(co_await client.hset("user:1000", "email", "john@example.com"));
    CO_EXPECT(co_await client.hset("user:1000", "age", "30"));
    std::cout << "HSET user:1000 (name, email, age)\n";

    auto name = co_await client.hget("user:1000", "name");
    if (name && *name) {
        std::cout << "HGET user:1000 name: " << **name << "\n";
    }

    auto allFields = co_await client.hgetall("user:1000");
    if (allFields) {
        std::cout << "HGETALL user:1000:\n";
        for (const auto& [field, value] : *allFields) {
            std::cout << "  " << field << " = " << value << "\n";
        }
    }
    std::cout << "\n";

    // List operations
    std::cout << "=== List Operations ===\n";

    co_await client.del("tasks");
    CO_EXPECT(co_await client.rpush("tasks", std::vector<std::string>{"task1", "task2", "task3"}));
    std::cout << "RPUSH tasks (task1, task2, task3)\n";

    auto listLen = co_await client.llen("tasks");
    if (listLen) std::cout << "LLEN tasks: " << *listLen << "\n";

    auto tasks = co_await client.lrange("tasks", 0, -1);
    if (tasks) {
        std::cout << "LRANGE tasks 0 -1:";
        for (const auto& task : *tasks) {
            std::cout << " " << task;
        }
        std::cout << "\n";
    }

    auto popped = co_await client.lpop("tasks");
    if (popped && *popped) {
        std::cout << "LPOP tasks: " << **popped << "\n\n";
    }

    // Set operations
    std::cout << "=== Set Operations ===\n";

    co_await client.del("tags");
    CO_EXPECT(co_await client.sadd("tags", std::vector<std::string>{"redis", "database", "cache", "nosql"}));
    std::cout << "SADD tags (redis, database, cache, nosql)\n";

    auto setSize = co_await client.scard("tags");
    if (setSize) std::cout << "SCARD tags: " << *setSize << "\n";

    auto isMember = co_await client.sismember("tags", "redis");
    if (isMember) {
        std::cout << "SISMEMBER tags 'redis': " << (*isMember ? "yes" : "no") << "\n";
    }

    auto members = co_await client.smembers("tags");
    if (members) {
        std::cout << "SMEMBERS tags:";
        for (const auto& m : *members) {
            std::cout << " " << m;
        }
        std::cout << "\n\n";
    }

    // Sorted set operations
    std::cout << "=== Sorted Set Operations ===\n";

    co_await client.del("leaderboard");
    std::vector<std::pair<double, std::string>> scores = {
        {100.0, "alice"},
        {85.5, "bob"},
        {92.0, "charlie"},
        {78.0, "dave"}
    };
    CO_EXPECT(co_await client.zadd("leaderboard", scores));
    std::cout << "ZADD leaderboard (alice:100, bob:85.5, charlie:92, dave:78)\n";

    auto ranking = co_await client.zrangeWithScores("leaderboard", 0, -1);
    if (ranking) {
        std::cout << "ZRANGE leaderboard 0 -1 WITHSCORES:\n";
        int rank = 1;
        for (const auto& [member, score] : *ranking) {
            std::cout << "  " << rank++ << ". " << member << ": " << std::fixed << std::setprecision(1) << score << "\n";
        }
    }

    auto aliceRank = co_await client.zrank("leaderboard", "alice");
    if (aliceRank && *aliceRank) {
        std::cout << "ZRANK leaderboard alice: " << **aliceRank << " (0-indexed)\n\n";
    }

    // Info
    std::cout << "=== Server Info ===\n";
    auto info = co_await client.info("server");
    if (info) {
        // Print first few lines
        std::string infoStr = *info;
        std::size_t pos = 0;
        int lines = 0;
        while (pos < infoStr.size() && lines < 5) {
            auto end = infoStr.find('\n', pos);
            if (end == std::string::npos) end = infoStr.size();
            std::cout << infoStr.substr(pos, end - pos) << "\n";
            pos = end + 1;
            lines++;
        }
        std::cout << "...\n\n";
    }

    // Cleanup
    std::cout << "=== Cleanup ===\n";
    co_await client.del("greeting");
    co_await client.del("temp_key");
    co_await client.del("counter");
    co_await client.del("user:1000");
    co_await client.del("tasks");
    co_await client.del("tags");
    co_await client.del("leaderboard");
    std::cout << "Cleaned up test keys\n";

    CO_EXPECT(co_await client.disconnect());
    std::cout << "\nDisconnected from Redis.\n";

    co_return {};
}
