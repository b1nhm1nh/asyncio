#ifndef ASYNCIO_MEMCACHED_CLIENT_H
#define ASYNCIO_MEMCACHED_CLIENT_H

#include <asyncio/task.h>
#include <asyncio/io.h>
#include <asyncio/net/stream.h>
#include <asyncio/net/tls.h>
#include <asyncio/channel.h>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <span>

namespace asyncio::memcached {

// Memcached Binary Protocol Implementation
// Reference: https://github.com/memcached/memcached/wiki/BinaryProtocolRevamped
    constexpr std::uint16_t DEFAULT_PORT = 11211;

    /// Memcached result codes
    enum class Result {
        SUCCESS,
        FAILURE,
        NOT_FOUND,
        NOT_STORED,
        EXISTS,
        TIMEOUT,
        CONNECTION_ERROR,
        SERVER_ERROR,
        CLIENT_ERROR
    };

    class ResultCategory final : public std::error_category {
    public:
        static const std::error_category &instance();

        [[nodiscard]] const char *name() const noexcept override {
            return "asyncio::memcached::Result";
        }

        [[nodiscard]] std::string message(int value) const override;
    };

    inline std::error_code make_error_code(Result e) {
        return {static_cast<int>(e), errorCategoryInstance<ResultCategory>()};
    }

    /// Stored value with metadata
    struct Value {
        std::vector<std::byte> data;
        std::uint32_t flags = 0;
        std::uint64_t cas = 0;  // CAS unique value

        /// Get data as string
        [[nodiscard]] std::string asString() const {
            return {reinterpret_cast<const char *>(data.data()), data.size()};
        }
    };

    /// Server info
    struct Server {
        std::string host;
        std::uint16_t port = DEFAULT_PORT;
        std::uint32_t weight = 1;

        Server(std::string h, std::uint16_t p = DEFAULT_PORT, std::uint32_t w = 1)
            : host(std::move(h)), port(p), weight(w) {}
    };

    /// Client configuration
    struct Config {
        std::vector<Server> servers;
        std::chrono::milliseconds connectTimeout{1000};
        std::chrono::milliseconds sendTimeout{0};
        std::chrono::milliseconds receiveTimeout{0};
        bool binaryProtocol = true;
        bool tcpNoDelay = true;
        bool tcpKeepAlive = true;

        /// Consistent hashing for server distribution
        bool consistentHashing = true;

        /// Number of distribution points per server
        std::uint32_t distributionPoints = 100;

        /// Auto remove dead servers
        bool autoRemoveFailedServers = true;

        /// Retry timeout for failed servers
        std::chrono::seconds serverFailureRetry{5};

        /// Connection pool size per server
        std::size_t poolSize = 1;
    };

    /// Stats for a single server
    struct ServerStats {
        std::string server;
        std::map<std::string, std::string> stats;
    };

    /// Memcached Client (native binary protocol implementation)
    class Client {
    public:
        DEFINE_ERROR_CODE_INNER(
            Error,
            "asyncio::memcached::Client",
            CONNECTION_FAILED, "failed to connect to memcached",
            NOT_CONNECTED, "client is not connected",
            TIMEOUT, "operation timed out",
            KEY_NOT_FOUND, "key not found",
            NOT_STORED, "value not stored",
            SERVER_ERROR, "server error",
            INVALID_ARGUMENTS, "invalid arguments"
        )

        Client();
        ~Client();

        Client(const Client &) = delete;
        Client &operator=(const Client &) = delete;
        Client(Client &&) noexcept;
        Client &operator=(Client &&) noexcept;

        /// Connect to memcached server(s)
        static task::Task<Client, std::error_code> connect(Config config);

        /// Connect to single server
        static task::Task<Client, std::error_code>
        connect(std::string host, std::uint16_t port = DEFAULT_PORT);

        /// Connect using connection string
        /// Format: "server1:port1,server2:port2,..."
        static task::Task<Client, std::error_code> connect(std::string connectionString);

        /// Check if connected
        [[nodiscard]] bool isConnected() const;

        /// Disconnect
        task::Task<void, std::error_code> disconnect();

        // ==================== Storage Commands ====================

        /// GET key
        task::Task<std::optional<Value>, std::error_code> get(std::string_view key);

        /// GET as string
        task::Task<std::optional<std::string>, std::error_code> getString(std::string_view key);

        /// MGET - get multiple keys
        task::Task<std::map<std::string, Value>, std::error_code>
        mget(std::span<const std::string> keys);

        /// SET key value [exptime] [flags]
        task::Task<void, std::error_code> set(
            std::string_view key,
            std::span<const std::byte> value,
            std::chrono::seconds exptime = std::chrono::seconds{0},
            std::uint32_t flags = 0
        );

        /// SET string value
        task::Task<void, std::error_code> set(
            std::string_view key,
            std::string_view value,
            std::chrono::seconds exptime = std::chrono::seconds{0},
            std::uint32_t flags = 0
        );

        /// ADD key value - store only if key doesn't exist
        task::Task<bool, std::error_code> add(
            std::string_view key,
            std::span<const std::byte> value,
            std::chrono::seconds exptime = std::chrono::seconds{0},
            std::uint32_t flags = 0
        );

        /// ADD string value
        task::Task<bool, std::error_code> add(
            std::string_view key,
            std::string_view value,
            std::chrono::seconds exptime = std::chrono::seconds{0},
            std::uint32_t flags = 0
        );

        /// REPLACE key value - store only if key exists
        task::Task<bool, std::error_code> replace(
            std::string_view key,
            std::span<const std::byte> value,
            std::chrono::seconds exptime = std::chrono::seconds{0},
            std::uint32_t flags = 0
        );

        /// REPLACE string value
        task::Task<bool, std::error_code> replace(
            std::string_view key,
            std::string_view value,
            std::chrono::seconds exptime = std::chrono::seconds{0},
            std::uint32_t flags = 0
        );

        /// APPEND key value
        task::Task<void, std::error_code> append(std::string_view key, std::string_view value);

        /// PREPEND key value
        task::Task<void, std::error_code> prepend(std::string_view key, std::string_view value);

        /// CAS - compare and swap (atomic update)
        task::Task<bool, std::error_code> cas(
            std::string_view key,
            std::span<const std::byte> value,
            std::uint64_t casUnique,
            std::chrono::seconds exptime = std::chrono::seconds{0},
            std::uint32_t flags = 0
        );

        // ==================== Delete Commands ====================

        /// DELETE key
        task::Task<bool, std::error_code> del(std::string_view key);

        /// DELETE multiple keys
        task::Task<std::int64_t, std::error_code> del(std::span<const std::string> keys);

        // ==================== Numeric Commands ====================

        /// INCR key delta
        task::Task<std::uint64_t, std::error_code>
        incr(std::string_view key, std::uint64_t delta = 1);

        /// DECR key delta
        task::Task<std::uint64_t, std::error_code>
        decr(std::string_view key, std::uint64_t delta = 1);

        // ==================== Touch Commands ====================

        /// TOUCH key exptime - update expiration without fetching
        task::Task<bool, std::error_code>
        touch(std::string_view key, std::chrono::seconds exptime);

        /// GAT (get and touch) - get value and update expiration
        task::Task<std::optional<Value>, std::error_code>
        gat(std::string_view key, std::chrono::seconds exptime);

        // ==================== Server Commands ====================

        /// FLUSH_ALL [delay]
        task::Task<void, std::error_code>
        flushAll(std::chrono::seconds delay = std::chrono::seconds{0});

        /// STATS [type]
        task::Task<std::vector<ServerStats>, std::error_code>
        stats(std::string_view type = "");

        /// VERSION
        task::Task<std::map<std::string, std::string>, std::error_code> version();

        /// QUIT (close connection)
        task::Task<void, std::error_code> quit();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

DECLARE_ERROR_CODES(asyncio::memcached::Result, asyncio::memcached::Client::Error)

#endif // ASYNCIO_MEMCACHED_CLIENT_H
