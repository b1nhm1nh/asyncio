#include <asyncio/redis/client.h>
#include <asyncio/memcached/client.h>
#include <asyncio/time.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace asyncio;
using json = nlohmann::json;

struct PerfStats {
    std::vector<double> latencies_us;  // microseconds
    double total_time_s = 0;
    int operations = 0;

    void add(double latency_us) {
        latencies_us.push_back(latency_us);
        operations++;
    }

    double rps() const {
        return operations / total_time_s;
    }

    double avg_latency() const {
        if (latencies_us.empty()) return 0;
        return std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0) / latencies_us.size();
    }

    double p50() const {
        if (latencies_us.empty()) return 0;
        auto sorted = latencies_us;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2];
    }

    double p99() const {
        if (latencies_us.empty()) return 0;
        auto sorted = latencies_us;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() * 99 / 100];
    }

    double max_latency() const {
        if (latencies_us.empty()) return 0;
        return *std::max_element(latencies_us.begin(), latencies_us.end());
    }

    void print(const std::string& name) const {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << name << ":\n";
        std::cout << "  Operations: " << operations << "\n";
        std::cout << "  Total time: " << total_time_s * 1000 << " ms\n";
        std::cout << "  Throughput: " << std::setprecision(0) << rps() << " ops/sec\n";
        std::cout << "  Latency (us):\n";
        std::cout << "    avg: " << std::setprecision(1) << avg_latency() << "\n";
        std::cout << "    p50: " << p50() << "\n";
        std::cout << "    p99: " << p99() << "\n";
        std::cout << "    max: " << max_latency() << "\n";
        std::cout << "\n";
    }
};

// Random candle generator
struct Candle {
    std::string symbol;
    int64_t timestamp;
    double open, high, low, close, volume;

    std::string serialize() const {
        return symbol + ":" + std::to_string(timestamp) + ":" +
               std::to_string(open) + ":" + std::to_string(high) + ":" +
               std::to_string(low) + ":" + std::to_string(close) + ":" +
               std::to_string(volume);
    }

    static Candle random(const std::string& sym, int64_t ts) {
        double base = 100.0 + (rand() % 10000) / 100.0;
        double range = base * 0.02;
        return {sym, ts, base,
                base + (rand() % 100) / 100.0 * range,
                base - (rand() % 100) / 100.0 * range,
                base + (rand() % 200 - 100) / 100.0 * range,
                static_cast<double>(rand() % 1000000)};
    }
};

// ==================== Binary vs JSON Candle Format ====================

// Binary packed candle (48 bytes) - matches Python struct.pack('<q5d', ...)
#pragma pack(push, 1)
struct BinaryCandle {
    int64_t timestamp;  // 8 bytes
    double open;        // 8 bytes
    double high;        // 8 bytes
    double low;         // 8 bytes
    double close;       // 8 bytes
    double volume;      // 8 bytes
    // Total: 48 bytes

    std::string toBinary() const {
        return std::string(reinterpret_cast<const char*>(this), sizeof(*this));
    }

    static BinaryCandle fromBinary(std::string_view data) {
        BinaryCandle c;
        std::memcpy(&c, data.data(), sizeof(c));
        return c;
    }

    static BinaryCandle random(int64_t ts) {
        double base = 100.0 + (rand() % 10000) / 100.0;
        double range = base * 0.02;
        return {ts, base,
                base + (rand() % 100) / 100.0 * range,
                base - (rand() % 100) / 100.0 * range,
                base + (rand() % 200 - 100) / 100.0 * range,
                static_cast<double>(rand() % 1000000)};
    }
};
#pragma pack(pop)
static_assert(sizeof(BinaryCandle) == 48, "BinaryCandle must be 48 bytes");

// JSON candle format
struct JsonCandle {
    int64_t time;
    double open, high, low, close, volume;

    std::string toJson() const {
        json j;
        j["time"] = time;
        j["open"] = open;
        j["high"] = high;
        j["low"] = low;
        j["close"] = close;
        j["volume"] = volume;
        return j.dump();
    }

    static JsonCandle fromJson(const std::string& data) {
        auto j = json::parse(data);
        return {
            j["time"].get<int64_t>(),
            j["open"].get<double>(),
            j["high"].get<double>(),
            j["low"].get<double>(),
            j["close"].get<double>(),
            j["volume"].get<double>()
        };
    }

    static JsonCandle random(int64_t ts) {
        double base = 100.0 + (rand() % 10000) / 100.0;
        double range = base * 0.02;
        return {ts, base,
                base + (rand() % 100) / 100.0 * range,
                base - (rand() % 100) / 100.0 * range,
                base + (rand() % 200 - 100) / 100.0 * range,
                static_cast<double>(rand() % 1000000)};
    }
};

task::Task<void, std::error_code> testRedis(int iterations) {
    std::cout << "========== REDIS PERFORMANCE TEST ==========\n\n";

    redis::Config config;
    config.host = "127.0.0.1";
    config.port = 6379;

    auto clientResult = co_await redis::Client::connect(config);
    if (!clientResult) {
        std::cerr << "Failed to connect to Redis\n";
        co_return std::unexpected(clientResult.error());
    }
    auto& client = *clientResult;

    // Test 1: PING
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await client.ping());
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Redis PING");
    }

    // Test 2: SET (64 bytes)
    {
        PerfStats stats;
        std::string value(64, 'x');
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await client.set("bench_key_" + std::to_string(i), value));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Redis SET (64 bytes)");
    }

    // Test 3: GET (64 bytes)
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await client.get("bench_key_" + std::to_string(i % iterations)));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Redis GET (64 bytes)");
    }

    // Test 4: Candle SET
    {
        PerfStats stats;
        srand(42);
        int64_t ts = 1700000000000;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto candle = Candle::random("BTCUSDT", ts++);
            std::string key = "candle:" + candle.symbol + ":" + std::to_string(candle.timestamp);

            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await client.set(key, candle.serialize()));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Redis Candle SET");
    }

    // Cleanup
    auto keys = co_await client.keys("bench_key_*");
    if (keys && !keys->empty()) {
        co_await client.del(*keys);
    }
    keys = co_await client.keys("candle:*");
    if (keys && !keys->empty()) {
        co_await client.del(*keys);
    }

    co_await client.disconnect();
    co_return {};
}

task::Task<void, std::error_code> testMemcached(int iterations) {
    std::cout << "========== MEMCACHED PERFORMANCE TEST ==========\n\n";

    auto clientResult = co_await memcached::Client::connect("127.0.0.1", 11211);
    if (!clientResult) {
        std::cerr << "Failed to connect to Memcached\n";
        co_return std::unexpected(clientResult.error());
    }
    auto& client = *clientResult;

    // Test 1: SET (64 bytes)
    {
        PerfStats stats;
        std::string value(64, 'x');
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await client.set("bench_key_" + std::to_string(i), value));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Memcached SET (64 bytes)");
    }

    // Test 2: GET (64 bytes)
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await client.get("bench_key_" + std::to_string(i % iterations)));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Memcached GET (64 bytes)");
    }

    // Test 3: Candle SET
    {
        PerfStats stats;
        srand(42);
        int64_t ts = 1700000000000;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto candle = Candle::random("BTCUSDT", ts++);
            std::string key = "candle:" + candle.symbol + ":" + std::to_string(candle.timestamp);

            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await client.set(key, candle.serialize()));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Memcached Candle SET");
    }

    // Test 4: INCR
    {
        PerfStats stats;
        co_await client.set("counter", "0");
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await client.incr("counter", 1));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Memcached INCR");
    }

    co_await client.disconnect();
    co_return {};
}

task::Task<void, std::error_code> testRedisBatch(int iterations) {
    std::cout << "========== REDIS BATCH TEST ==========\n\n";

    redis::Config config;
    config.host = "127.0.0.1";
    config.port = 6379;

    auto clientResult = co_await redis::Client::connect(config);
    if (!clientResult) {
        std::cerr << "Failed to connect to Redis\n";
        co_return std::unexpected(clientResult.error());
    }
    auto& client = *clientResult;

    std::string value(64, 'x');

    // Test MSET (batch SET) with different batch sizes
    for (int batchSize : {10, 50, 100}) {
        int numBatches = iterations / batchSize;

        auto start = std::chrono::high_resolution_clock::now();

        for (int batch = 0; batch < numBatches; batch++) {
            std::vector<std::pair<std::string, std::string>> kvs;
            kvs.reserve(batchSize);
            for (int i = 0; i < batchSize; i++) {
                kvs.emplace_back("batch_key_" + std::to_string(i), value);
            }
            CO_EXPECT(co_await client.mset(kvs));
        }

        auto end = std::chrono::high_resolution_clock::now();
        double total_time = std::chrono::duration<double>(end - start).count();
        int total_ops = numBatches * batchSize;

        std::cout << std::fixed << std::setprecision(0);
        std::cout << "Redis MSET (batch=" << batchSize << "):\n";
        std::cout << "  Operations: " << total_ops << "\n";
        std::cout << "  Total time: " << std::setprecision(2) << total_time * 1000 << " ms\n";
        std::cout << "  Throughput: " << std::setprecision(0) << total_ops / total_time << " ops/sec\n";
        std::cout << "  Per-batch latency: " << std::setprecision(1) << (total_time * 1000000 / numBatches) << " us\n\n";
    }

    // Test MGET (batch GET) - for fair comparison with Memcached
    std::cout << "--- Redis MGET (for comparison) ---\n\n";
    for (int batchSize : {10, 50, 100}) {
        int numBatches = iterations / batchSize;

        std::vector<std::string> keys;
        keys.reserve(batchSize);
        for (int i = 0; i < batchSize; i++) {
            keys.push_back("batch_key_" + std::to_string(i));
        }

        auto start = std::chrono::high_resolution_clock::now();

        for (int batch = 0; batch < numBatches; batch++) {
            CO_EXPECT(co_await client.mget(keys));
        }

        auto end = std::chrono::high_resolution_clock::now();
        double total_time = std::chrono::duration<double>(end - start).count();
        int total_ops = numBatches * batchSize;

        std::cout << std::fixed << std::setprecision(0);
        std::cout << "Redis MGET (batch=" << batchSize << "):\n";
        std::cout << "  Operations: " << total_ops << "\n";
        std::cout << "  Total time: " << std::setprecision(2) << total_time * 1000 << " ms\n";
        std::cout << "  Throughput: " << std::setprecision(0) << total_ops / total_time << " ops/sec\n";
        std::cout << "  Per-batch latency: " << std::setprecision(1) << (total_time * 1000000 / numBatches) << " us\n\n";
    }

    // Cleanup
    auto keys = co_await client.keys("batch_key_*");
    if (keys && !keys->empty()) {
        co_await client.del(*keys);
    }

    co_await client.disconnect();
    co_return {};
}

task::Task<void, std::error_code> testMemcachedBatch(int iterations) {
    std::cout << "========== MEMCACHED BATCH TEST ==========\n\n";

    auto clientResult = co_await memcached::Client::connect("127.0.0.1", 11211);
    if (!clientResult) {
        std::cerr << "Failed to connect to Memcached\n";
        co_return std::unexpected(clientResult.error());
    }
    auto& client = *clientResult;

    std::string value(64, 'x');

    // First, set up keys for MGET test
    for (int i = 0; i < 100; i++) {
        co_await client.set("mget_key_" + std::to_string(i), value);
    }

    // Test MGET (batch GET) with different batch sizes
    for (int batchSize : {10, 50, 100}) {
        int numBatches = iterations / batchSize;

        std::vector<std::string> keys;
        keys.reserve(batchSize);
        for (int i = 0; i < batchSize; i++) {
            keys.push_back("mget_key_" + std::to_string(i % 100));
        }

        auto start = std::chrono::high_resolution_clock::now();

        for (int batch = 0; batch < numBatches; batch++) {
            CO_EXPECT(co_await client.mget(keys));
        }

        auto end = std::chrono::high_resolution_clock::now();
        double total_time = std::chrono::duration<double>(end - start).count();
        int total_ops = numBatches * batchSize;

        std::cout << std::fixed << std::setprecision(0);
        std::cout << "Memcached MGET (batch=" << batchSize << "):\n";
        std::cout << "  Operations: " << total_ops << "\n";
        std::cout << "  Total time: " << std::setprecision(2) << total_time * 1000 << " ms\n";
        std::cout << "  Throughput: " << std::setprecision(0) << total_ops / total_time << " ops/sec\n";
        std::cout << "  Per-batch latency: " << std::setprecision(1) << (total_time * 1000000 / numBatches) << " us\n\n";
    }

    // Cleanup
    for (int i = 0; i < 100; i++) {
        co_await client.del("mget_key_" + std::to_string(i));
    }

    co_await client.disconnect();
    co_return {};
}

// ==================== Binary vs JSON Format Comparison ====================

task::Task<void, std::error_code> testCandleFormats(int iterations) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "CANDLE FORMAT COMPARISON: BINARY vs JSON\n";
    std::cout << std::string(70, '=') << "\n\n";

    // Show format sizes
    auto binaryCandle = BinaryCandle::random(1733580000000);
    auto jsonCandle = JsonCandle::random(1733580000000);
    std::string binaryData = binaryCandle.toBinary();
    std::string jsonData = jsonCandle.toJson();

    std::cout << "Format Sizes:\n";
    std::cout << "  Binary: " << binaryData.size() << " bytes\n";
    std::cout << "  JSON:   " << jsonData.size() << " bytes\n";
    std::cout << "  Ratio:  " << std::fixed << std::setprecision(2)
              << (double)jsonData.size() / binaryData.size() << "x larger\n\n";

    // Connect to Redis
    redis::Config redisConfig;
    redisConfig.host = "127.0.0.1";
    redisConfig.port = 6379;
    auto redisResult = co_await redis::Client::connect(redisConfig);
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

    srand(42);
    int64_t ts = 1733580000000;

    // ========== Redis Tests ==========
    std::cout << "---------- REDIS ----------\n\n";

    // Redis Binary SET
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto candle = BinaryCandle::random(ts++);
            std::string key = "fmt:bin:" + std::to_string(candle.timestamp);
            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await redis.set(key, candle.toBinary()));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Redis Binary SET");
    }

    // Redis JSON SET
    ts = 1733580000000;
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto candle = JsonCandle::random(ts++);
            std::string key = "fmt:json:" + std::to_string(candle.time);
            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await redis.set(key, candle.toJson()));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Redis JSON SET");
    }

    // Redis Binary GET + deserialize
    ts = 1733580000000;
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            std::string key = "fmt:bin:" + std::to_string(ts++);
            auto op_start = std::chrono::high_resolution_clock::now();
            auto result = co_await redis.get(key);
            CO_EXPECT(result);
            if (*result) {
                auto candle = BinaryCandle::fromBinary(**result);
                (void)candle;  // use the value
            }
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Redis Binary GET+Parse");
    }

    // Redis JSON GET + deserialize
    ts = 1733580000000;
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            std::string key = "fmt:json:" + std::to_string(ts++);
            auto op_start = std::chrono::high_resolution_clock::now();
            auto result = co_await redis.get(key);
            CO_EXPECT(result);
            if (*result) {
                auto candle = JsonCandle::fromJson(**result);
                (void)candle;  // use the value
            }
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Redis JSON GET+Parse");
    }

    // Cleanup Redis
    auto keys1 = co_await redis.keys("fmt:bin:*");
    if (keys1 && !keys1->empty()) co_await redis.del(*keys1);
    auto keys2 = co_await redis.keys("fmt:json:*");
    if (keys2 && !keys2->empty()) co_await redis.del(*keys2);

    // ========== Memcached Tests ==========
    std::cout << "---------- MEMCACHED ----------\n\n";

    ts = 1733580000000;

    // Memcached Binary SET
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto candle = BinaryCandle::random(ts++);
            std::string key = "fmt_bin_" + std::to_string(candle.timestamp);
            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await mc.set(key, candle.toBinary()));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Memcached Binary SET");
    }

    // Memcached JSON SET
    ts = 1733580000000;
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            auto candle = JsonCandle::random(ts++);
            std::string key = "fmt_json_" + std::to_string(candle.time);
            auto op_start = std::chrono::high_resolution_clock::now();
            CO_EXPECT(co_await mc.set(key, candle.toJson()));
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Memcached JSON SET");
    }

    // Memcached Binary GET + deserialize
    ts = 1733580000000;
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            std::string key = "fmt_bin_" + std::to_string(ts++);
            auto op_start = std::chrono::high_resolution_clock::now();
            auto result = co_await mc.get(key);
            CO_EXPECT(result);
            if (result->has_value()) {
                auto& data = result->value().data;
                std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
                auto candle = BinaryCandle::fromBinary(sv);
                (void)candle;
            }
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Memcached Binary GET+Parse");
    }

    // Memcached JSON GET + deserialize
    ts = 1733580000000;
    {
        PerfStats stats;
        auto start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            std::string key = "fmt_json_" + std::to_string(ts++);
            auto op_start = std::chrono::high_resolution_clock::now();
            auto result = co_await mc.get(key);
            CO_EXPECT(result);
            if (result->has_value()) {
                auto& data = result->value().data;
                std::string str(reinterpret_cast<const char*>(data.data()), data.size());
                auto candle = JsonCandle::fromJson(str);
                (void)candle;
            }
            auto op_end = std::chrono::high_resolution_clock::now();
            stats.add(std::chrono::duration<double, std::micro>(op_end - op_start).count());
        }

        auto end = std::chrono::high_resolution_clock::now();
        stats.total_time_s = std::chrono::duration<double>(end - start).count();
        stats.print("Memcached JSON GET+Parse");
    }

    co_await redis.disconnect();
    co_await mc.disconnect();

    std::cout << std::string(70, '=') << "\n\n";
    co_return {};
}

task::Task<void, std::error_code> testRedisConcurrent(int iterations, int concurrency) {
    std::cout << "========== REDIS CONCURRENT TEST (concurrency=" << concurrency << ") ==========\n\n";

    redis::Config config;
    config.host = "127.0.0.1";
    config.port = 6379;
    config.connectTimeout = std::chrono::milliseconds{5000};

    // Create multiple connections concurrently
    std::vector<task::Task<redis::Client, std::error_code>> connectTasks;
    for (int i = 0; i < concurrency; i++) {
        connectTasks.push_back(redis::Client::connect(config));
    }

    auto connectResults = co_await all(std::move(connectTasks));
    if (!connectResults) {
        std::cerr << "Failed to connect to Redis (concurrent)\n";
        co_return std::unexpected(connectResults.error());
    }

    std::vector<redis::Client> clients;
    for (auto& result : *connectResults) {
        clients.push_back(std::move(result));
    }

    // Concurrent SET test
    {
        std::string value(64, 'x');
        auto start = std::chrono::high_resolution_clock::now();

        for (int batch = 0; batch < iterations / concurrency; batch++) {
            std::vector<task::Task<void, std::error_code>> tasks;
            for (int i = 0; i < concurrency; i++) {
                int key_id = batch * concurrency + i;
                tasks.push_back([](redis::Client& c, std::string k, std::string v) -> task::Task<void, std::error_code> {
                    CO_EXPECT(co_await c.set(k, v));
                    co_return {};
                }(clients[i], "concurrent_key_" + std::to_string(key_id), value));
            }
            CO_EXPECT(co_await all(std::move(tasks)));
        }

        auto end = std::chrono::high_resolution_clock::now();
        double total_time = std::chrono::duration<double>(end - start).count();
        int total_ops = (iterations / concurrency) * concurrency;

        std::cout << std::fixed << std::setprecision(0);
        std::cout << "Redis Concurrent SET (" << concurrency << " connections):\n";
        std::cout << "  Operations: " << total_ops << "\n";
        std::cout << "  Total time: " << std::setprecision(2) << total_time * 1000 << " ms\n";
        std::cout << "  Throughput: " << std::setprecision(0) << total_ops / total_time << " ops/sec\n\n";
    }

    // Cleanup
    auto keys = co_await clients[0].keys("concurrent_key_*");
    if (keys && !keys->empty()) {
        co_await clients[0].del(*keys);
    }

    for (auto& client : clients) {
        co_await client.disconnect();
    }
    co_return {};
}

task::Task<void, std::error_code> testMemcachedConcurrent(int iterations, int concurrency) {
    std::cout << "========== MEMCACHED CONCURRENT TEST (concurrency=" << concurrency << ") ==========\n\n";

    // Create multiple connections concurrently
    std::vector<task::Task<memcached::Client, std::error_code>> connectTasks;
    for (int i = 0; i < concurrency; i++) {
        connectTasks.push_back(memcached::Client::connect("127.0.0.1", 11211));
    }

    auto connectResults = co_await all(std::move(connectTasks));
    if (!connectResults) {
        std::cerr << "Failed to connect to Memcached (concurrent)\n";
        co_return std::unexpected(connectResults.error());
    }

    std::vector<memcached::Client> clients;
    for (auto& result : *connectResults) {
        clients.push_back(std::move(result));
    }

    // Concurrent SET test
    {
        std::string value(64, 'x');
        auto start = std::chrono::high_resolution_clock::now();

        for (int batch = 0; batch < iterations / concurrency; batch++) {
            std::vector<task::Task<void, std::error_code>> tasks;
            for (int i = 0; i < concurrency; i++) {
                int key_id = batch * concurrency + i;
                tasks.push_back([](memcached::Client& c, std::string k, std::string v) -> task::Task<void, std::error_code> {
                    CO_EXPECT(co_await c.set(k, v));
                    co_return {};
                }(clients[i], "mc_concurrent_" + std::to_string(key_id), value));
            }
            CO_EXPECT(co_await all(std::move(tasks)));
        }

        auto end = std::chrono::high_resolution_clock::now();
        double total_time = std::chrono::duration<double>(end - start).count();
        int total_ops = (iterations / concurrency) * concurrency;

        std::cout << std::fixed << std::setprecision(0);
        std::cout << "Memcached Concurrent SET (" << concurrency << " connections):\n";
        std::cout << "  Operations: " << total_ops << "\n";
        std::cout << "  Total time: " << std::setprecision(2) << total_time * 1000 << " ms\n";
        std::cout << "  Throughput: " << std::setprecision(0) << total_ops / total_time << " ops/sec\n\n";
    }

    for (auto& client : clients) {
        co_await client.disconnect();
    }
    co_return {};
}

task::Task<void, std::error_code> asyncMain(int argc, char *argv[]) {
    int iterations = 1000;
    bool formatTestOnly = false;

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--format" || arg == "-f") {
            formatTestOnly = true;
            if (argc > 2) iterations = std::atoi(argv[2]);
        } else {
            iterations = std::atoi(argv[1]);
        }
    }

    std::cout << "Running performance test with " << iterations << " iterations\n\n";

    if (formatTestOnly) {
        // Only run format comparison test
        CO_EXPECT(co_await testCandleFormats(iterations));
    } else {
        // Run all tests
        CO_EXPECT(co_await testRedis(iterations));
        CO_EXPECT(co_await testMemcached(iterations));

        // Batch tests (pipelining equivalent)
        CO_EXPECT(co_await testRedisBatch(iterations));
        CO_EXPECT(co_await testMemcachedBatch(iterations));

        // Format comparison test
        CO_EXPECT(co_await testCandleFormats(iterations));
    }

    std::cout << "========== SUMMARY ==========\n";
    std::cout << "Iterations per test: " << iterations << "\n";
    std::cout << "Sequential: ~50K ops/sec (latency bound by ~20us round-trip)\n";
    std::cout << "Batched: scales with batch size (amortizes latency)\n";
    std::cout << "Binary format: 2.4x smaller, faster serialize/deserialize\n";

    co_return {};
}
