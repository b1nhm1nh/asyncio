#include <asyncio/memcached/client.h>
#include <asyncio/event_loop.h>
#include <benchmark/benchmark.h>

using namespace asyncio;
using namespace asyncio::memcached;

static Client* gMemcachedClient = nullptr;

static void setupMemcached() {
    if (gMemcachedClient) return;

    auto result = asyncio::run([]() -> task::Task<Client*, std::error_code> {
        auto clientResult = co_await Client::connect("127.0.0.1", 11211);
        if (!clientResult) {
            co_return nullptr;
        }

        auto* client = new Client(std::move(*clientResult));
        co_return client;
    });

    if (result && *result && **result) {
        gMemcachedClient = **result;
    }
}

static void teardownMemcached() {
    if (gMemcachedClient) {
        asyncio::run([&]() -> task::Task<void, std::error_code> {
            co_await gMemcachedClient->disconnect();
            co_return {};
        });
        delete gMemcachedClient;
        gMemcachedClient = nullptr;
    }
}

// Benchmark: Memcached SET
static void BM_Memcached_Set(benchmark::State& state) {
    setupMemcached();
    if (!gMemcachedClient) {
        state.SkipWithError("Memcached not available");
        return;
    }

    std::string key = "bench_key";
    std::string value(state.range(0), 'x');  // Value of specified size

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gMemcachedClient->set(key, value));
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
        co_await gMemcachedClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Memcached_Set)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// Benchmark: Memcached GET
static void BM_Memcached_Get(benchmark::State& state) {
    setupMemcached();
    if (!gMemcachedClient) {
        state.SkipWithError("Memcached not available");
        return;
    }

    std::string key = "bench_get_key";
    std::string value(state.range(0), 'x');

    // Setup: store value first
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gMemcachedClient->set(key, value);
        co_return {};
    });

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            auto getResult = co_await gMemcachedClient->get(key);
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
        co_await gMemcachedClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Memcached_Get)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// Benchmark: Memcached INCR
static void BM_Memcached_Incr(benchmark::State& state) {
    setupMemcached();
    if (!gMemcachedClient) {
        state.SkipWithError("Memcached not available");
        return;
    }

    std::string key = "bench_counter";

    // Setup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gMemcachedClient->set(key, "0");
        co_return {};
    });

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gMemcachedClient->incr(key, 1));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("INCR failed");
            return;
        }
    }

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gMemcachedClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Memcached_Incr);

// Benchmark: Memcached SET/GET cycle
static void BM_Memcached_SetGet(benchmark::State& state) {
    setupMemcached();
    if (!gMemcachedClient) {
        state.SkipWithError("Memcached not available");
        return;
    }

    std::string key = "bench_setget";
    std::string value(state.range(0), 'x');

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gMemcachedClient->set(key, value));
            CO_EXPECT(co_await gMemcachedClient->get(key));
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
        co_await gMemcachedClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Memcached_SetGet)->Arg(64)->Arg(256)->Arg(1024);

// Benchmark: Memcached ADD/DELETE
static void BM_Memcached_AddDelete(benchmark::State& state) {
    setupMemcached();
    if (!gMemcachedClient) {
        state.SkipWithError("Memcached not available");
        return;
    }

    std::string key = "bench_add";
    std::string value(256, 'x');

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gMemcachedClient->add(key, value));
            CO_EXPECT(co_await gMemcachedClient->del(key));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("ADD/DELETE failed");
            return;
        }
    }
}
BENCHMARK(BM_Memcached_AddDelete);

// Benchmark: Memcached APPEND
static void BM_Memcached_Append(benchmark::State& state) {
    setupMemcached();
    if (!gMemcachedClient) {
        state.SkipWithError("Memcached not available");
        return;
    }

    std::string key = "bench_append";
    std::string appendValue(64, 'x');

    // Setup: create initial value
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gMemcachedClient->set(key, "initial");
        co_return {};
    });

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gMemcachedClient->append(key, appendValue));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("APPEND failed");
            return;
        }
    }

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gMemcachedClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Memcached_Append);

// ==================== Candle Data Benchmarks ====================

struct MCCandle {
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

    static MCCandle random(const std::string& sym, int64_t ts) {
        double base = 100.0 + (rand() % 10000) / 100.0;
        double range = base * 0.02;
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
static void BM_Memcached_Candle_Set(benchmark::State& state) {
    setupMemcached();
    if (!gMemcachedClient) {
        state.SkipWithError("Memcached not available");
        return;
    }

    srand(42);
    int64_t ts = 1700000000000;

    for (auto _ : state) {
        auto candle = MCCandle::random("BTCUSDT", ts++);
        std::string key = "candle:" + candle.symbol + ":" + std::to_string(candle.timestamp);

        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            CO_EXPECT(co_await gMemcachedClient->set(key, candle.serialize()));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("SET candle failed");
            return;
        }
    }
}
BENCHMARK(BM_Memcached_Candle_Set);

// Benchmark: Get and set candle (read-modify-write pattern)
static void BM_Memcached_Candle_GetSet(benchmark::State& state) {
    setupMemcached();
    if (!gMemcachedClient) {
        state.SkipWithError("Memcached not available");
        return;
    }

    srand(42);
    std::string key = "candle:bench:BTCUSDT";

    // Store initial candle
    auto initialCandle = MCCandle::random("BTCUSDT", 1700000000000);
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gMemcachedClient->set(key, initialCandle.serialize());
        co_return {};
    });

    int64_t ts = 1700000000001;

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            // Get current
            CO_EXPECT(co_await gMemcachedClient->get(key));
            // Update with new candle
            auto newCandle = MCCandle::random("BTCUSDT", ts++);
            CO_EXPECT(co_await gMemcachedClient->set(key, newCandle.serialize()));
            co_return {};
        });

        if (!result || !*result) {
            state.SkipWithError("GET/SET candle failed");
            return;
        }
    }

    // Cleanup
    asyncio::run([&]() -> task::Task<void, std::error_code> {
        co_await gMemcachedClient->del(key);
        co_return {};
    });
}
BENCHMARK(BM_Memcached_Candle_GetSet);

// Benchmark: Store multiple candles for different symbols
static void BM_Memcached_Candle_MultiSymbol(benchmark::State& state) {
    setupMemcached();
    if (!gMemcachedClient) {
        state.SkipWithError("Memcached not available");
        return;
    }

    const std::vector<std::string> symbols = {
        "BTCUSDT", "ETHUSDT", "BNBUSDT", "SOLUSDT", "XRPUSDT",
        "ADAUSDT", "DOGEUSDT", "AVAXUSDT", "DOTUSDT", "LINKUSDT"
    };

    srand(42);
    int64_t ts = 1700000000000;

    for (auto _ : state) {
        auto result = asyncio::run([&]() -> task::Task<void, std::error_code> {
            for (const auto& sym : symbols) {
                auto candle = MCCandle::random(sym, ts);
                std::string key = "candle:" + sym + ":" + std::to_string(ts);
                CO_EXPECT(co_await gMemcachedClient->set(key, candle.serialize()));
            }
            co_return {};
        });

        ts++;

        if (!result || !*result) {
            state.SkipWithError("Multi-symbol SET failed");
            return;
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()) * symbols.size());
}
BENCHMARK(BM_Memcached_Candle_MultiSymbol);

// Cleanup function to be called by main
void cleanupMemcached() {
    teardownMemcached();
}
