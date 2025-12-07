#include <asyncio/redis/client.h>
#include <asyncio/event_loop.h>
#include <benchmark/benchmark.h>

using namespace asyncio;
using namespace asyncio::redis;

static Client* gRedisClient = nullptr;

static void setupRedis() {
    if (gRedisClient) return;

    auto result = asyncio::run([]() -> task::Task<Client*, std::error_code> {
        Config config;
        config.host = "127.0.0.1";
        config.port = 6379;

        auto clientResult = co_await Client::connect(config);
        if (!clientResult) {
            co_return nullptr;
        }

        auto* client = new Client(std::move(*clientResult));
        co_return client;
    });

    if (result && *result && **result) {
        gRedisClient = **result;
    }
}

static void teardownRedis() {
    if (gRedisClient) {
        asyncio::run([&]() -> task::Task<void, std::error_code> {
            co_await gRedisClient->disconnect();
            co_return {};
        });
        delete gRedisClient;
        gRedisClient = nullptr;
    }
}

// Benchmark: Redis PING
static void BM_Redis_Ping(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gRedisClient->ping());
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("PING failed");
            return;
        }
    }
}
BENCHMARK(BM_Redis_Ping);

// Benchmark: Redis SET
static void BM_Redis_Set(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    std::string key = "bench_key";
    std::string value(state.range(0), 'x');  // Value of specified size

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gRedisClient->set(key, value));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("SET failed");
            return;
        }
    }

    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(value.size()));

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gRedisClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Redis_Set)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// Benchmark: Redis GET
static void BM_Redis_Get(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    std::string key = "bench_get_key";
    std::string value(state.range(0), 'x');

    // Setup: store value first
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gRedisClient->set(key, value);
        co_return {};
    });

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            auto getResult = co_await gRedisClient->get(key);
            CO_EXPECT(getResult);
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("GET failed");
            return;
        }
    }

    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(value.size()));

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gRedisClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Redis_Get)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// Benchmark: Redis INCR
static void BM_Redis_Incr(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    std::string key = "bench_counter";

    // Setup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gRedisClient->set(key, "0");
        co_return {};
    });

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gRedisClient->incr(key));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("INCR failed");
            return;
        }
    }

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gRedisClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Redis_Incr);

// Benchmark: Redis SET/GET cycle
static void BM_Redis_SetGet(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    std::string key = "bench_setget";
    std::string value(state.range(0), 'x');

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gRedisClient->set(key, value));
            CO_EXPECT(co_await gRedisClient->get(key));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("SET/GET failed");
            return;
        }
    }

    state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(value.size()) * 2);

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gRedisClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Redis_SetGet)->Arg(64)->Arg(256)->Arg(1024);

// Benchmark: Redis HSET/HGET
static void BM_Redis_Hash(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    std::string key = "bench_hash";
    std::string field = "field1";
    std::string value(256, 'x');

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gRedisClient->hset(key, field, value));
            CO_EXPECT(co_await gRedisClient->hget(key, field));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("HSET/HGET failed");
            return;
        }
    }

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gRedisClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Redis_Hash);

// Benchmark: Redis LPUSH/LPOP
static void BM_Redis_List(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    std::string key = "bench_list";

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gRedisClient->lpush(key, std::vector<std::string>{"item"}));
            CO_EXPECT(co_await gRedisClient->lpop(key));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("LPUSH/LPOP failed");
            return;
        }
    }

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gRedisClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Redis_List);

// ==================== Candle Data Benchmarks ====================

struct Candle {
    std::string symbol;
    int64_t timestamp;
    double open;
    double high;
    double low;
    double close;
    double volume;

    std::string serialize() const {
        return symbol + ":" + std::to_string(timestamp) + ":" +
               std::to_string(open) + ":" + std::to_string(high) + ":" +
               std::to_string(low) + ":" + std::to_string(close) + ":" +
               std::to_string(volume);
    }

    static Candle random(const std::string& sym, int64_t ts) {
        double base = 100.0 + (rand() % 10000) / 100.0;
        double range = base * 0.02;  // 2% range
        return {
            sym, ts,
            base,
            base + (rand() % 100) / 100.0 * range,
            base - (rand() % 100) / 100.0 * range,
            base + (rand() % 200 - 100) / 100.0 * range,
            static_cast<double>(rand() % 1000000)
        };
    }
};

// Benchmark: Store single candle
static void BM_Redis_Candle_Set(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    srand(42);
    int64_t ts = 1700000000000;

    for (auto _ : state) {
        auto candle = Candle::random("BTCUSDT", ts++);
        std::string key = "candle:" + candle.symbol + ":" + std::to_string(candle.timestamp);

        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gRedisClient->set(key, candle.serialize()));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("SET candle failed");
            return;
        }
    }

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        auto keys = co_await gRedisClient->keys("candle:*");
        if (keys && !keys->empty()) {
            co_await gRedisClient->del(*keys);
        }
        co_return {};
    });
}
BENCHMARK(BM_Redis_Candle_Set);

// Benchmark: Store candle as hash
static void BM_Redis_Candle_HSet(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    srand(42);
    int64_t ts = 1700000000000;

    for (auto _ : state) {
        auto candle = Candle::random("ETHUSDT", ts++);
        std::string key = "candle:hash:" + candle.symbol + ":" + std::to_string(candle.timestamp);

        std::vector<std::pair<std::string, std::string>> fields = {
            {"open", std::to_string(candle.open)},
            {"high", std::to_string(candle.high)},
            {"low", std::to_string(candle.low)},
            {"close", std::to_string(candle.close)},
            {"volume", std::to_string(candle.volume)}
        };

        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gRedisClient->hset(key, fields));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("HSET candle failed");
            return;
        }
    }

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        auto keys = co_await gRedisClient->keys("candle:hash:*");
        if (keys && !keys->empty()) {
            co_await gRedisClient->del(*keys);
        }
        co_return {};
    });
}
BENCHMARK(BM_Redis_Candle_HSet);

// Benchmark: Store candles in sorted set (for time series)
static void BM_Redis_Candle_ZAdd(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    srand(42);
    int64_t ts = 1700000000000;
    std::string key = "candles:zset:BTCUSDT";

    for (auto _ : state) {
        auto candle = Candle::random("BTCUSDT", ts++);

        std::vector<std::pair<double, std::string>> members = {
            {static_cast<double>(candle.timestamp), candle.serialize()}
        };

        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gRedisClient->zadd(key, members));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("ZADD candle failed");
            return;
        }
    }

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gRedisClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Redis_Candle_ZAdd);

// Benchmark: Batch store candles (pipeline simulation)
static void BM_Redis_Candle_Batch(benchmark::State& state) {
    setupRedis();
    if (!gRedisClient) {
        state.SkipWithError("Redis not available");
        return;
    }

    const int batchSize = state.range(0);
    srand(42);
    int64_t ts = 1700000000000;

    for (auto _ : state) {
        std::vector<std::pair<std::string, std::string>> kvs;
        kvs.reserve(batchSize);

        for (int i = 0; i < batchSize; i++) {
            auto candle = Candle::random("BTCUSDT", ts++);
            std::string key = "batch:candle:" + std::to_string(candle.timestamp);
            kvs.emplace_back(key, candle.serialize());
        }

        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gRedisClient->mset(kvs));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("MSET batch failed");
            return;
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()) * batchSize);

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        auto keys = co_await gRedisClient->keys("batch:candle:*");
        if (keys && !keys->empty()) {
            co_await gRedisClient->del(*keys);
        }
        co_return {};
    });
}
BENCHMARK(BM_Redis_Candle_Batch)->Arg(10)->Arg(50)->Arg(100);

// Cleanup function to be called by main
void cleanupRedis() {
    teardownRedis();
}
