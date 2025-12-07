/**
 * vertix-candle-binance-usdtm - Binance USDT-M Futures Candle Worker
 *
 * Subscribes to Binance USDT-M testnet/mainnet WebSocket, generates candles
 * from trade data, and stores them in Redis using binary format.
 *
 * Binary Format (48 bytes, little-endian):
 *   - timestamp: int64 (8 bytes) - candle open time in milliseconds
 *   - open: double (8 bytes)
 *   - high: double (8 bytes)
 *   - low: double (8 bytes)
 *   - close: double (8 bytes)
 *   - volume: double (8 bytes)
 *
 * Redis Key Format: CANDLE:{prefix}:{SYMBOL}:{timeframe}
 * Example: CANDLE:BINANCE_TESTNET_USDTM:BTCUSDT:1s
 *
 * Usage:
 *   ./asyncio_candle --config config/candle_worker.json
 *   ./asyncio_candle BTCUSDT,ETHUSDT  # legacy mode
 */

#include <asyncio/http/websocket.h>
#include <asyncio/redis/client.h>
#include <asyncio/candle/candle_manager.h>
#include <asyncio/time.h>
#include <asyncio/fs.h>
#include <zero/cmdline.h>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <nlohmann/json.hpp>
#include <csignal>
#include <atomic>
#include <mutex>
#include <cstring>
#include <fstream>
#include <set>
#include <optional>

using json = nlohmann::json;
using namespace asyncio::candle;

// Global shutdown flag
static std::atomic<bool> g_shutdown{false};

// ============================================================================
// Configuration
// ============================================================================

struct WorkerConfig {
    // Exchange settings
    std::string exchange = "binance";
    std::string market = "usdtm";
    bool testnet = true;

    // Redis settings
    std::string redisHost = "127.0.0.1";
    uint16_t redisPort = 6379;
    int redisDb = 0;
    std::string redisPrefix = "CANDLE";
    int maxCandles = 1000;

    // Reconnection settings
    int reconnectDelaySec = 5;
    int maxReconnectAttempts = 0;  // 0 = unlimited

    // Symbols
    std::vector<std::string> symbols;

    // Timeframes (as milliseconds)
    std::vector<int64_t> timeframes;

    // Logging
    std::set<int64_t> consoleTimeframes;
    bool showEmpty = true;

    // Derived
    std::string keyPrefix() const {
        std::string env = testnet ? "TESTNET" : "";
        std::string marketUpper = market;
        std::transform(marketUpper.begin(), marketUpper.end(), marketUpper.begin(), ::toupper);

        if (testnet) {
            return fmt::format("{}_{}_{}",
                exchange == "binance" ? "BINANCE" : exchange,
                "TESTNET", marketUpper);
        }
        return fmt::format("{}_{}",
            exchange == "binance" ? "BINANCE" : exchange, marketUpper);
    }
};

// ============================================================================
// Timeframe parsing
// ============================================================================

int64_t parseTimeframe(const std::string& tf) {
    static const std::map<std::string, int64_t> timeframes = {
        {"1s", 1000}, {"3s", 3000}, {"5s", 5000}, {"15s", 15000}, {"30s", 30000},
        {"1m", 60000}, {"5m", 300000}, {"15m", 900000}, {"30m", 1800000},
        {"1h", 3600000}, {"4h", 14400000}, {"8h", 28800000}, {"12h", 43200000},
        {"1D", 86400000}, {"1d", 86400000},
    };
    auto it = timeframes.find(tf);
    return it != timeframes.end() ? it->second : 0;
}

std::string intervalToString(int64_t intervalMs) {
    static const std::map<int64_t, std::string> timeframes = {
        {1000, "1s"}, {3000, "3s"}, {5000, "5s"}, {15000, "15s"}, {30000, "30s"},
        {60000, "1m"}, {300000, "5m"}, {900000, "15m"}, {1800000, "30m"},
        {3600000, "1h"}, {14400000, "4h"}, {28800000, "8h"}, {43200000, "12h"},
        {86400000, "1D"},
    };
    auto it = timeframes.find(intervalMs);
    if (it != timeframes.end()) return it->second;

    if (intervalMs < 60000) return fmt::format("{}s", intervalMs / 1000);
    if (intervalMs < 3600000) return fmt::format("{}m", intervalMs / 60000);
    if (intervalMs < 86400000) return fmt::format("{}h", intervalMs / 3600000);
    return fmt::format("{}D", intervalMs / 86400000);
}

// ============================================================================
// Config loading
// ============================================================================

std::expected<WorkerConfig, std::string> loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::unexpected(fmt::format("Cannot open config file: {}", path));
    }

    try {
        json j = json::parse(file);
        WorkerConfig cfg;

        // Exchange settings
        if (j.contains("exchange")) cfg.exchange = j["exchange"];
        if (j.contains("market")) cfg.market = j["market"];
        if (j.contains("testnet")) cfg.testnet = j["testnet"];

        // Redis settings
        if (j.contains("redis")) {
            auto& r = j["redis"];
            if (r.contains("host")) cfg.redisHost = r["host"];
            if (r.contains("port")) cfg.redisPort = r["port"];
            if (r.contains("db")) cfg.redisDb = r["db"];
            if (r.contains("prefix")) cfg.redisPrefix = r["prefix"];
            if (r.contains("max_candles")) cfg.maxCandles = r["max_candles"];
        }

        // Reconnection settings
        if (j.contains("reconnect")) {
            auto& rc = j["reconnect"];
            if (rc.contains("delay_sec")) cfg.reconnectDelaySec = rc["delay_sec"];
            if (rc.contains("max_attempts")) cfg.maxReconnectAttempts = rc["max_attempts"];
        }

        // Symbols
        if (j.contains("symbols")) {
            for (const auto& s : j["symbols"]) {
                std::string sym = s;
                std::transform(sym.begin(), sym.end(), sym.begin(), ::toupper);
                cfg.symbols.push_back(sym);
            }
        }

        // Timeframes
        if (j.contains("timeframes")) {
            for (const auto& tf : j["timeframes"]) {
                int64_t ms = parseTimeframe(tf);
                if (ms > 0) cfg.timeframes.push_back(ms);
            }
        }

        // Logging
        if (j.contains("logging")) {
            auto& log = j["logging"];
            if (log.contains("console_timeframes")) {
                for (const auto& tf : log["console_timeframes"]) {
                    int64_t ms = parseTimeframe(tf);
                    if (ms > 0) cfg.consoleTimeframes.insert(ms);
                }
            }
            if (log.contains("show_empty")) cfg.showEmpty = log["show_empty"];
        }

        // Default timeframes if not specified
        if (cfg.timeframes.empty()) {
            cfg.timeframes = {1000, 3000, 5000, 15000, 30000, 60000, 300000,
                              900000, 1800000, 3600000, 14400000, 28800000,
                              43200000, 86400000};
        }

        // Default console timeframes
        if (cfg.consoleTimeframes.empty()) {
            cfg.consoleTimeframes.insert(1000);  // 1s by default
        }

        return cfg;

    } catch (const std::exception& e) {
        return std::unexpected(fmt::format("Config parse error: {}", e.what()));
    }
}

// ============================================================================
// Binary Candle Format (48 bytes) - Compatible with Python client
// ============================================================================

#pragma pack(push, 1)
struct BinaryCandle {
    int64_t timestamp;
    double open;
    double high;
    double low;
    double close;
    double volume;

    std::string toBinary() const {
        return std::string(reinterpret_cast<const char*>(this), sizeof(*this));
    }

    static BinaryCandle fromCandle(const Candle& c) {
        return BinaryCandle{
            .timestamp = c.openTimeMs,
            .open = c.open, .high = c.high, .low = c.low,
            .close = c.close, .volume = c.volume
        };
    }
};
#pragma pack(pop)
static_assert(sizeof(BinaryCandle) == 48, "BinaryCandle must be 48 bytes");

// ============================================================================
// Redis Binary Candle Storage with Reconnection
// ============================================================================

class RedisCandleStorage {
public:
    RedisCandleStorage(asyncio::redis::Config config, std::string prefix,
                       int maxCandles = 1000, int reconnectBaseDelay = 3, int reconnectMaxDelay = 10)
        : mConfig(std::move(config))
        , mPrefix(std::move(prefix))
        , mMaxCandles(maxCandles)
        , mReconnectBaseDelay(reconnectBaseDelay)
        , mReconnectMaxDelay(reconnectMaxDelay)
    {}

    asyncio::task::Task<bool, std::error_code> connect() {
        fmt::print("Connecting to Redis ({}:{} db={})...\n",
            mConfig.host, mConfig.port, mConfig.database);

        auto result = co_await asyncio::redis::Client::connect(mConfig);
        if (!result) {
            fmt::print(stderr, "Redis connection failed: {}\n", result.error().message());
            mConnected = false;
            co_return false;
        }

        // Use emplace to avoid extra moves
        mClient.emplace(std::move(*result));

        if (!mClient->isConnected()) {
            fmt::print(stderr, "Redis client not connected after connect()\n");
            mConnected = false;
            mClient.reset();
            co_return false;
        }

        mConnected = true;
        mCurrentRetryDelay = 0;  // Reset delay on successful connection
        fmt::print("Redis connected\n");
        co_return true;
    }

    asyncio::task::Task<bool, std::error_code> reconnect() {
        mConnected = false;

        // Just reset - don't try to call disconnect() on potentially broken client
        // The destructor will clean up resources
        mClient.reset();

        // Exponential backoff: first try immediately, then 3s, 6s, 9s... up to max
        if (mCurrentRetryDelay > 0) {
            fmt::print("Redis reconnecting in {}s...\n", mCurrentRetryDelay);
            co_await asyncio::sleep(std::chrono::seconds{mCurrentRetryDelay});
        } else {
            fmt::print("Redis reconnecting immediately...\n");
        }

        auto connected = co_await connect();
        if (!connected || !*connected) {
            // Increase delay for next retry
            if (mCurrentRetryDelay == 0) {
                mCurrentRetryDelay = mReconnectBaseDelay;
            } else {
                mCurrentRetryDelay = std::min(mCurrentRetryDelay + mReconnectBaseDelay, mReconnectMaxDelay);
            }
        }

        co_return connected;
    }

    asyncio::task::Task<void, std::error_code> storeCandle(
        const std::string& symbol, int64_t intervalMs, const Candle& candle)
    {
        // Ensure connected
        if (!mConnected || !mClient || !mClient->isConnected()) {
            mConnected = false;
            auto reconnected = co_await reconnect();
            if (!reconnected || !*reconnected) {
                co_return std::unexpected(std::make_error_code(std::errc::not_connected));
            }
        }

        std::string key = fmt::format("{}:{}:{}:{}", mRedisPrefix, mPrefix, symbol, intervalToString(intervalMs));
        BinaryCandle bc = BinaryCandle::fromCandle(candle);

        std::vector<std::pair<double, std::string>> members = {
            {static_cast<double>(bc.timestamp), bc.toBinary()}
        };

        auto zaddResult = co_await mClient->zadd(key, members);
        if (!zaddResult) {
            fmt::print(stderr, "Redis store failed: {}\n", zaddResult.error().message());
            mConnected = false;
            co_return std::unexpected(zaddResult.error());
        }

        // Trim if needed
        if (mConnected && mClient) {
            auto countResult = co_await mClient->zcard(key);
            if (countResult && *countResult > mMaxCandles) {
                co_await mClient->command({
                    "ZREMRANGEBYRANK", key, "0", std::to_string(*countResult - mMaxCandles - 1)
                });
            }
        }

        mCandlesStored++;
        co_return {};
    }

    asyncio::task::Task<void, std::error_code> disconnect() {
        mConnected = false;
        if (mClient && mClient->isConnected()) {
            co_await mClient->disconnect();
        }
        mClient.reset();
        co_return {};
    }

    void setRedisPrefix(const std::string& prefix) { mRedisPrefix = prefix; }
    uint64_t candlesStored() const { return mCandlesStored; }
    bool isConnected() const { return mConnected && mClient && mClient->isConnected(); }

private:
    asyncio::redis::Config mConfig;
    std::optional<asyncio::redis::Client> mClient;
    std::string mPrefix;
    std::string mRedisPrefix = "CANDLE";
    int mMaxCandles;
    int mReconnectBaseDelay;
    int mReconnectMaxDelay;
    int mCurrentRetryDelay = 0;
    bool mConnected = false;
    std::atomic<uint64_t> mCandlesStored{0};
};

// ============================================================================
// Utilities
// ============================================================================

inline int64_t currentTimestampMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void signalHandler(int) {
    fmt::print("\nShutdown requested...\n");
    g_shutdown = true;
}

// ============================================================================
// Main
// ============================================================================

asyncio::task::Task<void, std::error_code> asyncMain(const int argc, char *argv[]) {
    zero::Cmdline cmdline;
    cmdline.addOptional<std::string>("config", 'c', "Config file path", "");
    cmdline.addOptional<std::string>("symbols", 's', "Comma-separated symbols (legacy)", "");
    cmdline.addOptional<std::string>("testnet", 't', "Use testnet (legacy)", "true");
    cmdline.parse(argc, argv);

    WorkerConfig cfg;
    auto configPath = cmdline.getOptional<std::string>("config");

    if (configPath && !configPath->empty()) {
        // Load from config file
        auto result = loadConfig(*configPath);
        if (!result) {
            fmt::print(stderr, "Error: {}\n", result.error());
            co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        cfg = *result;
    } else {
        // Legacy command line mode
        auto symbolsStr = cmdline.getOptional<std::string>("symbols");
        if (!symbolsStr || symbolsStr->empty()) {
            fmt::print(stderr, "Usage: {} --config <config.json>\n", argv[0]);
            fmt::print(stderr, "   or: {} --symbols BTCUSDT,ETHUSDT [--testnet true]\n", argv[0]);
            co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }

        auto testnetStr = cmdline.getOptional<std::string>("testnet");
        cfg.testnet = (*testnetStr == "true" || *testnetStr == "1");

        std::istringstream ss(*symbolsStr);
        std::string sym;
        while (std::getline(ss, sym, ',')) {
            sym.erase(0, sym.find_first_not_of(" \t"));
            sym.erase(sym.find_last_not_of(" \t") + 1);
            if (!sym.empty()) {
                std::transform(sym.begin(), sym.end(), sym.begin(), ::toupper);
                cfg.symbols.push_back(sym);
            }
        }

        // Default timeframes
        cfg.timeframes = {1000, 3000, 5000, 15000, 30000, 60000, 300000,
                          900000, 1800000, 3600000, 14400000, 28800000,
                          43200000, 86400000};
        cfg.consoleTimeframes = {1000};
    }

    if (cfg.symbols.empty()) {
        fmt::print(stderr, "No symbols configured\n");
        co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    // Build lowercase symbols for WebSocket
    std::vector<std::string> symbolsLower;
    for (const auto& s : cfg.symbols) {
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        symbolsLower.push_back(lower);
    }

    std::string keyPrefix = cfg.keyPrefix();

    // Print configuration
    fmt::print("============================================================\n");
    fmt::print("BINANCE USDT-M CANDLE WORKER ({})\n", cfg.testnet ? "TESTNET" : "MAINNET");
    fmt::print("============================================================\n");
    fmt::print("Symbols: ");
    for (size_t i = 0; i < cfg.symbols.size(); ++i) {
        if (i > 0) fmt::print(", ");
        fmt::print("{}", cfg.symbols[i]);
    }
    fmt::print("\n");
    fmt::print("Redis: {}:{} db={}\n", cfg.redisHost, cfg.redisPort, cfg.redisDb);
    fmt::print("Key format: {}:{}:{{SYMBOL}}:{{TF}}\n", cfg.redisPrefix, keyPrefix);
    fmt::print("Max candles: {}\n", cfg.maxCandles);
    fmt::print("Timeframes: ");
    for (size_t i = 0; i < cfg.timeframes.size(); ++i) {
        if (i > 0) fmt::print(",");
        fmt::print("{}", intervalToString(cfg.timeframes[i]));
    }
    fmt::print("\n");
    fmt::print("Console: ");
    for (auto it = cfg.consoleTimeframes.begin(); it != cfg.consoleTimeframes.end(); ++it) {
        if (it != cfg.consoleTimeframes.begin()) fmt::print(",");
        fmt::print("{}", intervalToString(*it));
    }
    fmt::print(" (show_empty={})\n", cfg.showEmpty);
    fmt::print("Redis reconnect: immediate, then 3s->6s->9s->10s max\n");
    fmt::print("============================================================\n\n");

    // Create Redis config
    asyncio::redis::Config redisConfig;
    redisConfig.host = cfg.redisHost;
    redisConfig.port = cfg.redisPort;
    redisConfig.database = cfg.redisDb;

    // Create storage with reconnection support (immediate first retry, then 3s->6s->9s->10s max)
    RedisCandleStorage storage(redisConfig, keyPrefix, cfg.maxCandles, 3, 10);
    storage.setRedisPrefix(cfg.redisPrefix);

    // Initial Redis connection
    auto connected = co_await storage.connect();
    if (!connected || !*connected) {
        fmt::print(stderr, "Initial Redis connection failed, will retry on first store\n");
    }

    std::atomic<uint64_t> tradesProcessed{0};

    // Pending candles queue
    struct PendingCandle { std::string symbol; int64_t intervalMs; Candle candle; };
    std::vector<PendingCandle> pendingCandles;
    std::mutex pendingMutex;

    // Create candle manager
    CandleManager candleManager(
        [&](const std::string& symbolLower, int64_t intervalMs, const Candle& c) {
            std::string symbolUpper = symbolLower;
            std::transform(symbolUpper.begin(), symbolUpper.end(), symbolUpper.begin(), ::toupper);

            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                pendingCandles.push_back({symbolUpper, intervalMs, c});
            }

            // Console logging based on config
            if (cfg.consoleTimeframes.count(intervalMs) && (cfg.showEmpty || c.tradeCount > 0)) {
                auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(c.openTimeMs));
                fmt::print("[{}] {} {} O:{:.2f} H:{:.2f} L:{:.2f} C:{:.2f} V:{:.4f} T={}\n",
                    intervalToString(intervalMs), symbolUpper, tp,
                    c.open, c.high, c.low, c.close, c.volume, c.tradeCount);
            }
        },
        true  // Emit empty candles
    );

    // Subscribe symbols
    for (const auto& sym : symbolsLower) {
        candleManager.subscribe(sym, cfg.timeframes);
    }

    // Build WebSocket URL
    std::string wsBaseUrl = cfg.testnet
        ? "https://stream.binancefuture.com:443/stream"
        : "https://fstream.binance.com:443/stream";

    std::string streams;
    for (size_t i = 0; i < symbolsLower.size(); ++i) {
        if (i > 0) streams += "/";
        streams += symbolsLower[i] + "@aggTrade";
    }

    auto urlResult = asyncio::http::URL::from(wsBaseUrl);
    if (!urlResult) {
        fmt::print(stderr, "Failed to parse URL\n");
        co_return std::unexpected{urlResult.error()};
    }
    auto url = std::move(*urlResult);
    url.query("streams=" + streams);

    fmt::print("Connecting to Binance...\n");

    // Main loop with reconnection
    while (!g_shutdown) {
        auto ws = co_await asyncio::http::ws::WebSocket::connect(url);

        if (!ws) {
            fmt::print(stderr, "WebSocket failed: {}\n", ws.error().message());
            if (g_shutdown) break;
            fmt::print("Binance reconnecting in {}s...\n", cfg.reconnectDelaySec);
            co_await asyncio::sleep(std::chrono::seconds{cfg.reconnectDelaySec});
            continue;
        }

        fmt::print("WebSocket connected! Receiving trades...\n\n");

        // Heartbeat task
        auto heartbeatTask = [&]() -> asyncio::task::Task<void, std::error_code> {
            while (!g_shutdown) {
                co_await asyncio::sleep(std::chrono::milliseconds{500});
                candleManager.tick(currentTimestampMs());

                std::vector<PendingCandle> toStore;
                { std::lock_guard<std::mutex> lock(pendingMutex); toStore.swap(pendingCandles); }

                for (const auto& pc : toStore) {
                    auto result = co_await storage.storeCandle(pc.symbol, pc.intervalMs, pc.candle);
                    if (!result) {
                        fmt::print(stderr, "Store failed: {}\n", result.error().message());
                    }
                }
            }
            co_return {};
        };

        // Message task
        auto processTask = [&]() -> asyncio::task::Task<void, std::error_code> {
            while (!g_shutdown) {
                auto message = co_await ws->readMessage();
                if (!message) {
                    if (message.error() != asyncio::http::ws::CloseCode::NORMAL_CLOSURE) {
                        fmt::print(stderr, "WS error: {}\n", message.error().message());
                    }
                    break;
                }
                if (message->opcode != asyncio::http::ws::Opcode::TEXT) continue;

                try {
                    auto j = json::parse(std::get<std::string>(message->data));
                    if (!j.contains("data")) continue;
                    auto& data = j["data"];
                    if (data["e"] != "aggTrade") continue;

                    std::string symbol = data["s"];
                    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::tolower);
                    double price = std::stod(data["p"].get<std::string>());
                    double qty = std::stod(data["q"].get<std::string>());
                    int64_t timestampMs = data["T"];

                    candleManager.onTrade(symbol, price, qty, timestampMs);
                    tradesProcessed++;
                } catch (...) {}
            }
            co_return {};
        };

        co_await asyncio::task::allSettled(heartbeatTask(), processTask());

        if (!g_shutdown) {
            fmt::print("Binance connection lost. Reconnecting in {}s...\n", cfg.reconnectDelaySec);
            co_await asyncio::sleep(std::chrono::seconds{cfg.reconnectDelaySec});
        }
    }

    candleManager.flush();

    fmt::print("\n============================================================\n");
    fmt::print("STATISTICS\n");
    fmt::print("Trades: {} | Candles stored: {}\n", tradesProcessed.load(), storage.candlesStored());
    fmt::print("============================================================\n");

    co_await storage.disconnect();
    fmt::print("Disconnected\n");

    co_return {};
}
