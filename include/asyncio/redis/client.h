#ifndef ASYNCIO_REDIS_CLIENT_H
#define ASYNCIO_REDIS_CLIENT_H

#include <asyncio/task.h>
#include <asyncio/promise.h>
#include <asyncio/event_loop.h>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <span>

namespace asyncio::redis {
    constexpr std::uint16_t DEFAULT_PORT = 6379;
    constexpr std::uint16_t DEFAULT_TLS_PORT = 6380;

    /// Redis value types
    using Nil = std::monostate;
    using String = std::string;
    using Integer = std::int64_t;
    using Double = double;
    using Boolean = bool;
    struct Error {
        std::string type;
        std::string message;
    };

    /// Forward declare for recursive variant
    struct Array;
    struct Map;

    /// Redis reply value (RESP3)
    using Value = std::variant<
        Nil,
        String,
        Integer,
        Double,
        Boolean,
        Error,
        std::unique_ptr<Array>,
        std::unique_ptr<Map>
    >;

    struct Array {
        std::vector<Value> elements;
    };

    struct Map {
        std::vector<std::pair<Value, Value>> entries;
    };

    /// Optional string result
    using OptionalString = std::optional<std::string>;

    /// Client configuration
    struct Config {
        std::string host = "127.0.0.1";
        std::uint16_t port = DEFAULT_PORT;
        std::string password;
        std::string username;  // Redis 6.0+ ACL
        int database = 0;
        std::chrono::milliseconds connectTimeout{5000};
        std::chrono::milliseconds commandTimeout{0};  // 0 = no timeout
        bool autoReconnect = true;
        std::size_t maxReconnectAttempts = 3;
        std::chrono::milliseconds reconnectDelay{1000};
    };

    /// TLS configuration
    struct TLSConfig {
        std::string certFile;
        std::string keyFile;
        std::string caFile;
        std::string caPath;
        bool verifyPeer = true;
        std::string serverName;  // SNI
    };

    /// Pub/Sub message
    struct PubSubMessage {
        enum class Type { MESSAGE, PMESSAGE, SUBSCRIBE, UNSUBSCRIBE, PSUBSCRIBE, PUNSUBSCRIBE };
        Type type;
        std::string channel;
        std::string pattern;  // For PMESSAGE
        std::string payload;
        std::int64_t subscriberCount = 0;  // For (un)subscribe confirmations
    };

    /// Scan result
    template<typename T>
    struct ScanResult {
        std::string cursor;
        std::vector<T> keys;

        bool finished() const { return cursor == "0"; }
    };

    /// Redis Client
    class Client {
    public:
        DEFINE_ERROR_CODE_INNER(
            Error,
            "asyncio::redis::Client",
            CONNECTION_FAILED, "failed to connect to redis",
            AUTHENTICATION_FAILED, "redis authentication failed",
            COMMAND_FAILED, "redis command failed",
            TIMEOUT, "operation timed out",
            NOT_CONNECTED, "client is not connected",
            INVALID_RESPONSE, "invalid response from redis",
            PROTOCOL_ERROR, "redis protocol error",
            IO_ERROR, "i/o error",
            CONNECTION_LOST, "connection lost"
        )

        Client();
        ~Client();

        Client(const Client &) = delete;
        Client &operator=(const Client &) = delete;
        Client(Client &&) noexcept;
        Client &operator=(Client &&) noexcept;

        /// Connect to Redis server
        static task::Task<Client, std::error_code> connect(Config config);

        /// Connect with TLS
        static task::Task<Client, std::error_code> connect(Config config, TLSConfig tlsConfig);

        /// Connect using URL (redis://[[user]:password@]host[:port][/database])
        static task::Task<Client, std::error_code> connect(std::string url);

        /// Check if connected
        [[nodiscard]] bool isConnected() const;

        /// Disconnect
        task::Task<void, std::error_code> disconnect();

        // ==================== Generic Commands ====================

        /// Execute raw command
        task::Task<Value, std::error_code> command(std::vector<std::string> args);

        /// Execute command with string result
        task::Task<OptionalString, std::error_code> commandString(std::vector<std::string> args);

        /// Execute command with integer result
        task::Task<std::int64_t, std::error_code> commandInt(std::vector<std::string> args);

        // ==================== String Commands ====================

        /// GET key
        task::Task<OptionalString, std::error_code> get(std::string_view key);

        /// SET key value [EX seconds] [PX milliseconds] [NX|XX]
        task::Task<bool, std::error_code> set(
            std::string_view key,
            std::string_view value,
            std::chrono::milliseconds ttl = std::chrono::milliseconds{0},
            bool nx = false,
            bool xx = false
        );

        /// SETEX key seconds value
        task::Task<void, std::error_code> setex(
            std::string_view key,
            std::chrono::seconds ttl,
            std::string_view value
        );

        /// SETNX key value
        task::Task<bool, std::error_code> setnx(std::string_view key, std::string_view value);

        /// MGET key [key ...]
        task::Task<std::vector<OptionalString>, std::error_code>
        mget(std::span<const std::string> keys);

        /// MSET key value [key value ...]
        task::Task<void, std::error_code>
        mset(std::span<const std::pair<std::string, std::string>> kvs);

        /// INCR key
        task::Task<std::int64_t, std::error_code> incr(std::string_view key);

        /// INCRBY key increment
        task::Task<std::int64_t, std::error_code> incrby(std::string_view key, std::int64_t delta);

        /// DECR key
        task::Task<std::int64_t, std::error_code> decr(std::string_view key);

        /// APPEND key value
        task::Task<std::int64_t, std::error_code> append(std::string_view key, std::string_view value);

        // ==================== Key Commands ====================

        /// DEL key [key ...]
        task::Task<std::int64_t, std::error_code> del(std::span<const std::string> keys);

        /// DEL single key
        task::Task<bool, std::error_code> del(std::string_view key);

        /// EXISTS key [key ...]
        task::Task<std::int64_t, std::error_code> exists(std::span<const std::string> keys);

        /// EXISTS single key
        task::Task<bool, std::error_code> exists(std::string_view key);

        /// EXPIRE key seconds
        task::Task<bool, std::error_code> expire(std::string_view key, std::chrono::seconds seconds);

        /// PEXPIRE key milliseconds
        task::Task<bool, std::error_code> pexpire(std::string_view key, std::chrono::milliseconds ms);

        /// TTL key
        task::Task<std::int64_t, std::error_code> ttl(std::string_view key);

        /// PTTL key
        task::Task<std::int64_t, std::error_code> pttl(std::string_view key);

        /// KEYS pattern
        task::Task<std::vector<std::string>, std::error_code> keys(std::string_view pattern);

        /// SCAN cursor [MATCH pattern] [COUNT count]
        task::Task<ScanResult<std::string>, std::error_code>
        scan(std::string_view cursor, std::string_view pattern = "*", std::size_t count = 10);

        /// TYPE key
        task::Task<std::string, std::error_code> type(std::string_view key);

        /// RENAME key newkey
        task::Task<void, std::error_code> rename(std::string_view key, std::string_view newKey);

        // ==================== Hash Commands ====================

        /// HGET key field
        task::Task<OptionalString, std::error_code> hget(std::string_view key, std::string_view field);

        /// HSET key field value [field value ...]
        task::Task<std::int64_t, std::error_code>
        hset(std::string_view key, std::span<const std::pair<std::string, std::string>> fieldValues);

        /// HSET single field
        task::Task<bool, std::error_code>
        hset(std::string_view key, std::string_view field, std::string_view value);

        /// HMGET key field [field ...]
        task::Task<std::vector<OptionalString>, std::error_code>
        hmget(std::string_view key, std::span<const std::string> fields);

        /// HGETALL key
        task::Task<std::vector<std::pair<std::string, std::string>>, std::error_code>
        hgetall(std::string_view key);

        /// HDEL key field [field ...]
        task::Task<std::int64_t, std::error_code>
        hdel(std::string_view key, std::span<const std::string> fields);

        /// HEXISTS key field
        task::Task<bool, std::error_code> hexists(std::string_view key, std::string_view field);

        /// HLEN key
        task::Task<std::int64_t, std::error_code> hlen(std::string_view key);

        /// HINCRBY key field increment
        task::Task<std::int64_t, std::error_code>
        hincrby(std::string_view key, std::string_view field, std::int64_t delta);

        // ==================== List Commands ====================

        /// LPUSH key element [element ...]
        task::Task<std::int64_t, std::error_code>
        lpush(std::string_view key, std::span<const std::string> elements);

        /// RPUSH key element [element ...]
        task::Task<std::int64_t, std::error_code>
        rpush(std::string_view key, std::span<const std::string> elements);

        /// LPOP key
        task::Task<OptionalString, std::error_code> lpop(std::string_view key);

        /// RPOP key
        task::Task<OptionalString, std::error_code> rpop(std::string_view key);

        /// LRANGE key start stop
        task::Task<std::vector<std::string>, std::error_code>
        lrange(std::string_view key, std::int64_t start, std::int64_t stop);

        /// LLEN key
        task::Task<std::int64_t, std::error_code> llen(std::string_view key);

        /// BLPOP key [key ...] timeout
        task::Task<std::optional<std::pair<std::string, std::string>>, std::error_code>
        blpop(std::span<const std::string> keys, std::chrono::seconds timeout);

        /// BRPOP key [key ...] timeout
        task::Task<std::optional<std::pair<std::string, std::string>>, std::error_code>
        brpop(std::span<const std::string> keys, std::chrono::seconds timeout);

        // ==================== Set Commands ====================

        /// SADD key member [member ...]
        task::Task<std::int64_t, std::error_code>
        sadd(std::string_view key, std::span<const std::string> members);

        /// SREM key member [member ...]
        task::Task<std::int64_t, std::error_code>
        srem(std::string_view key, std::span<const std::string> members);

        /// SMEMBERS key
        task::Task<std::vector<std::string>, std::error_code> smembers(std::string_view key);

        /// SISMEMBER key member
        task::Task<bool, std::error_code> sismember(std::string_view key, std::string_view member);

        /// SCARD key
        task::Task<std::int64_t, std::error_code> scard(std::string_view key);

        // ==================== Sorted Set Commands ====================

        /// ZADD key [NX|XX] [GT|LT] [CH] [INCR] score member [score member ...]
        task::Task<std::int64_t, std::error_code>
        zadd(std::string_view key, std::span<const std::pair<double, std::string>> scoreMembers,
             bool nx = false, bool xx = false, bool ch = false);

        /// ZRANGE key start stop [WITHSCORES]
        task::Task<std::vector<std::string>, std::error_code>
        zrange(std::string_view key, std::int64_t start, std::int64_t stop);

        /// ZRANGE with scores
        task::Task<std::vector<std::pair<std::string, double>>, std::error_code>
        zrangeWithScores(std::string_view key, std::int64_t start, std::int64_t stop);

        /// ZRANK key member
        task::Task<std::optional<std::int64_t>, std::error_code>
        zrank(std::string_view key, std::string_view member);

        /// ZSCORE key member
        task::Task<std::optional<double>, std::error_code>
        zscore(std::string_view key, std::string_view member);

        /// ZCARD key
        task::Task<std::int64_t, std::error_code> zcard(std::string_view key);

        /// ZREM key member [member ...]
        task::Task<std::int64_t, std::error_code>
        zrem(std::string_view key, std::span<const std::string> members);

        // ==================== Pub/Sub Commands ====================

        /// PUBLISH channel message
        task::Task<std::int64_t, std::error_code>
        publish(std::string_view channel, std::string_view message);

        /// SUBSCRIBE channel [channel ...]
        task::Task<void, std::error_code> subscribe(std::span<const std::string> channels);

        /// UNSUBSCRIBE [channel ...]
        task::Task<void, std::error_code> unsubscribe(std::span<const std::string> channels = {});

        /// PSUBSCRIBE pattern [pattern ...]
        task::Task<void, std::error_code> psubscribe(std::span<const std::string> patterns);

        /// PUNSUBSCRIBE [pattern ...]
        task::Task<void, std::error_code> punsubscribe(std::span<const std::string> patterns = {});

        /// Receive pub/sub message (blocks until message received)
        task::Task<PubSubMessage, std::error_code> receivePubSub();

        // ==================== Server Commands ====================

        /// PING [message]
        task::Task<std::string, std::error_code> ping(std::string_view message = "");

        /// ECHO message
        task::Task<std::string, std::error_code> echo(std::string_view message);

        /// INFO [section]
        task::Task<std::string, std::error_code> info(std::string_view section = "");

        /// DBSIZE
        task::Task<std::int64_t, std::error_code> dbsize();

        /// FLUSHDB [ASYNC|SYNC]
        task::Task<void, std::error_code> flushdb(bool async = false);

        /// FLUSHALL [ASYNC|SYNC]
        task::Task<void, std::error_code> flushall(bool async = false);

        /// SELECT index
        task::Task<void, std::error_code> select(int database);

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };

    /// URL parsing helper
    struct URL {
        std::string scheme;  // "redis" or "rediss"
        std::string host = "127.0.0.1";
        std::uint16_t port = DEFAULT_PORT;
        std::string username;
        std::string password;
        int database = 0;

        static std::expected<URL, std::error_code> parse(std::string_view url);
    };
}

DECLARE_ERROR_CODE(asyncio::redis::Client::Error)

#endif // ASYNCIO_REDIS_CLIENT_H
