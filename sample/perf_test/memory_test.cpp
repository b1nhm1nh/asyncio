#include <asyncio/redis/client.h>
#include <asyncio/memcached/client.h>
#include <asyncio/time.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <iomanip>
#include <cstring>

using namespace asyncio;
using json = nlohmann::json;

#pragma pack(push, 1)
struct BinaryCandle {
    int64_t timestamp;
    double open, high, low, close, volume;

    std::string toBinary() const {
        return std::string(reinterpret_cast<const char*>(this), sizeof(*this));
    }
};
#pragma pack(pop)

task::Task<void, std::error_code> asyncMain(int argc, char *argv[]) {
    const int NUM_CANDLES = 10000;

    std::cout << "Memory Comparison Test: " << NUM_CANDLES << " candles\n";
    std::cout << std::string(60, '=') << "\n\n";

    // Connect to Redis
    redis::Config config;
    config.host = "127.0.0.1";
    config.port = 6379;
    auto redisResult = co_await redis::Client::connect(config);
    if (!redisResult) {
        std::cerr << "Failed to connect to Redis\n";
        co_return std::unexpected(redisResult.error());
    }
    auto& redis = *redisResult;

    // Connect to Memcached
    auto mcResult = co_await memcached::Client::connect("127.0.0.1", 11211);
    if (!mcResult) {
        std::cerr << "Failed to connect to Memcached\n";
        co_return std::unexpected(mcResult.error());
    }
    auto& mc = *mcResult;

    // Get baseline memory
    auto getRedisMemory = [&]() -> task::Task<int64_t, std::error_code> {
        // GCC 15 ICE workaround: explicit vector instead of brace-init
        std::vector<std::string> infoArgs{"INFO", "memory"};
        auto result = co_await redis.command(std::move(infoArgs));
        CO_EXPECT(result);
        auto info = std::get<std::string>(*result);
        auto pos = info.find("used_memory:");
        if (pos != std::string::npos) {
            auto end = info.find("\r\n", pos);
            auto val = info.substr(pos + 12, end - pos - 12);
            co_return std::stoll(val);
        }
        co_return 0;
    };

    auto getMcMemory = [&]() -> task::Task<int64_t, std::error_code> {
        auto result = co_await mc.stats();
        CO_EXPECT(result);
        if (!result->empty()) {
            auto& serverStats = result->front();
            auto it = serverStats.stats.find("bytes");
            if (it != serverStats.stats.end()) {
                co_return std::stoll(it->second);
            }
        }
        co_return 0;
    };

    // Flush all
    std::vector<std::string> flushArgs{"FLUSHALL"};
    co_await redis.command(std::move(flushArgs));
    co_await sleep(std::chrono::milliseconds{100});

    auto redisBase = co_await getRedisMemory();
    auto mcBase = co_await getMcMemory();

    std::cout << "Baseline Memory:\n";
    std::cout << "  Redis:     " << std::setw(12) << *redisBase << " bytes\n";
    std::cout << "  Memcached: " << std::setw(12) << *mcBase << " bytes\n\n";

    // Test 1: Redis Sorted Set with Binary data
    std::cout << "Storing " << NUM_CANDLES << " Binary candles in Redis Sorted Set...\n";
    for (int i = 0; i < NUM_CANDLES; i++) {
        BinaryCandle candle = {1733580000000 + i, 100.0 + i*0.01, 101.0, 99.0, 100.5, 1000.0};
        std::vector<std::pair<double, std::string>> members = {
            {static_cast<double>(candle.timestamp), candle.toBinary()}
        };
        CO_EXPECT(co_await redis.zadd("mem:binary:candles", members));
    }

    auto redisAfterBinary = co_await getRedisMemory();
    int64_t redisBinaryUsed = *redisAfterBinary - *redisBase;
    std::cout << "  Used: " << redisBinaryUsed << " bytes ("
              << std::fixed << std::setprecision(1) << (double)redisBinaryUsed / NUM_CANDLES
              << " bytes/candle)\n\n";

    // Test 2: Redis Sorted Set with JSON data
    std::cout << "Storing " << NUM_CANDLES << " JSON candles in Redis Sorted Set...\n";
    for (int i = 0; i < NUM_CANDLES; i++) {
        json j;
        j["time"] = 1733580000000 + i;
        j["open"] = 100.0 + i*0.01;
        j["high"] = 101.0;
        j["low"] = 99.0;
        j["close"] = 100.5;
        j["volume"] = 1000.0;

        std::vector<std::pair<double, std::string>> members = {
            {static_cast<double>(1733580000000 + i), j.dump()}
        };
        CO_EXPECT(co_await redis.zadd("mem:json:candles", members));
    }

    auto redisAfterJson = co_await getRedisMemory();
    int64_t redisJsonUsed = *redisAfterJson - *redisAfterBinary;
    std::cout << "  Used: " << redisJsonUsed << " bytes ("
              << std::fixed << std::setprecision(1) << (double)redisJsonUsed / NUM_CANDLES
              << " bytes/candle)\n\n";

    // Test 3: Memcached with Binary data
    std::cout << "Storing " << NUM_CANDLES << " Binary candles in Memcached...\n";
    for (int i = 0; i < NUM_CANDLES; i++) {
        BinaryCandle candle = {1733580000000 + i, 100.0 + i*0.01, 101.0, 99.0, 100.5, 1000.0};
        std::string key = "mc_bin_" + std::to_string(candle.timestamp);
        CO_EXPECT(co_await mc.set(key, candle.toBinary()));
    }

    auto mcAfterBinary = co_await getMcMemory();
    int64_t mcBinaryUsed = *mcAfterBinary - *mcBase;
    std::cout << "  Used: " << mcBinaryUsed << " bytes ("
              << std::fixed << std::setprecision(1) << (double)mcBinaryUsed / NUM_CANDLES
              << " bytes/candle)\n\n";

    // Test 4: Memcached with JSON data
    std::cout << "Storing " << NUM_CANDLES << " JSON candles in Memcached...\n";
    for (int i = 0; i < NUM_CANDLES; i++) {
        json j;
        j["time"] = 1733580000000 + i;
        j["open"] = 100.0 + i*0.01;
        j["high"] = 101.0;
        j["low"] = 99.0;
        j["close"] = 100.5;
        j["volume"] = 1000.0;

        std::string key = "mc_json_" + std::to_string(1733580000000 + i);
        CO_EXPECT(co_await mc.set(key, j.dump()));
    }

    auto mcAfterJson = co_await getMcMemory();
    int64_t mcJsonUsed = *mcAfterJson - *mcAfterBinary;
    std::cout << "  Used: " << mcJsonUsed << " bytes ("
              << std::fixed << std::setprecision(1) << (double)mcJsonUsed / NUM_CANDLES
              << " bytes/candle)\n\n";

    // Summary
    std::cout << std::string(60, '=') << "\n";
    std::cout << "SUMMARY (" << NUM_CANDLES << " candles)\n";
    std::cout << std::string(60, '=') << "\n\n";

    std::cout << std::setw(25) << "Storage" << std::setw(15) << "Total" << std::setw(15) << "Per Candle\n";
    std::cout << std::string(55, '-') << "\n";
    std::cout << std::setw(25) << "Redis Binary (ZSET)" << std::setw(15) << redisBinaryUsed << std::setw(15) << std::fixed << std::setprecision(1) << (double)redisBinaryUsed/NUM_CANDLES << "\n";
    std::cout << std::setw(25) << "Redis JSON (ZSET)" << std::setw(15) << redisJsonUsed << std::setw(15) << (double)redisJsonUsed/NUM_CANDLES << "\n";
    std::cout << std::setw(25) << "Memcached Binary" << std::setw(15) << mcBinaryUsed << std::setw(15) << (double)mcBinaryUsed/NUM_CANDLES << "\n";
    std::cout << std::setw(25) << "Memcached JSON" << std::setw(15) << mcJsonUsed << std::setw(15) << (double)mcJsonUsed/NUM_CANDLES << "\n";
    std::cout << std::string(55, '-') << "\n\n";

    std::cout << "Ratios:\n";
    std::cout << "  Redis JSON/Binary: " << std::setprecision(2) << (double)redisJsonUsed/redisBinaryUsed << "x\n";
    std::cout << "  Memcached JSON/Binary: " << (double)mcJsonUsed/mcBinaryUsed << "x\n";
    std::cout << "  Memcached/Redis (Binary): " << (double)mcBinaryUsed/redisBinaryUsed << "x\n";

    // Cleanup
    std::vector<std::string> delArgs{"DEL", "mem:binary:candles", "mem:json:candles"};
    co_await redis.command(std::move(delArgs));
    co_await redis.disconnect();
    co_await mc.disconnect();

    co_return {};
}
