#include <asyncio/redis/client.h>
#include <asyncio/net/stream.h>
#include <asyncio/net/tls.h>
#include <asyncio/sync/mutex.h>
#include <asyncio/event_loop.h>
#include <asyncio/buffer.h>
#include <asyncio/time.h>
#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libuv.h>
#include <regex>
#include <charconv>
#include <cstdlib>

namespace asyncio::redis {

// ==================== Error Category ====================

const std::error_category &Client::ErrorCategory::instance() {
    return errorCategoryInstance<Client::ErrorCategory>();
}

// ==================== URL Parsing ====================

std::expected<URL, std::error_code> URL::parse(std::string_view urlStr) {
    URL url;

    std::string urlString(urlStr);
    // redis://[[user:]password@]host[:port][/database]
    std::regex urlRegex(R"(^(redis|rediss)://(?:(?:([^:@]+)(?::([^@]*))?@)?)?([^:/]+)(?::(\d+))?(?:/(\d+))?$)");
    std::smatch match;

    if (!std::regex_match(urlString, match, urlRegex)) {
        return std::unexpected(make_error_code(Client::Error::CONNECTION_FAILED));
    }

    url.scheme = match[1].str();

    if (match[2].matched) {
        url.username = match[2].str();
    }

    if (match[3].matched) {
        url.password = match[3].str();
    } else if (match[2].matched && !url.username.empty()) {
        // If only one part before @, it's the password
        url.password = url.username;
        url.username.clear();
    }

    url.host = match[4].str();

    if (match[5].matched) {
        url.port = static_cast<std::uint16_t>(std::stoi(match[5].str()));
    } else {
        url.port = (url.scheme == "rediss") ? DEFAULT_TLS_PORT : DEFAULT_PORT;
    }

    if (match[6].matched) {
        url.database = std::stoi(match[6].str());
    }

    return url;
}

// ==================== Helper Functions ====================

namespace {

Value parseRedisReply(redisReply *reply);

Value parseRedisReply(redisReply *reply) {
    if (!reply) {
        return Nil{};
    }

    switch (reply->type) {
        case REDIS_REPLY_NIL:
            return Nil{};

        case REDIS_REPLY_STRING:
        case REDIS_REPLY_STATUS:
            return String{reply->str, reply->len};

        case REDIS_REPLY_ERROR: {
            Error err;
            std::string errStr{reply->str, reply->len};
            auto spacePos = errStr.find(' ');
            if (spacePos != std::string::npos) {
                err.type = errStr.substr(0, spacePos);
                err.message = errStr.substr(spacePos + 1);
            } else {
                err.type = "ERR";
                err.message = errStr;
            }
            return err;
        }

        case REDIS_REPLY_INTEGER:
            return Integer{reply->integer};

        case REDIS_REPLY_DOUBLE:
            return Double{reply->dval};

        case REDIS_REPLY_BOOL:
            return Boolean{reply->integer != 0};

        case REDIS_REPLY_ARRAY:
        case REDIS_REPLY_SET:
        case REDIS_REPLY_PUSH: {
            auto arr = std::make_unique<Array>();
            arr->elements.reserve(reply->elements);
            for (size_t i = 0; i < reply->elements; ++i) {
                arr->elements.push_back(parseRedisReply(reply->element[i]));
            }
            return arr;
        }

        case REDIS_REPLY_MAP: {
            auto map = std::make_unique<Map>();
            map->entries.reserve(reply->elements / 2);
            for (size_t i = 0; i + 1 < reply->elements; i += 2) {
                map->entries.emplace_back(
                    parseRedisReply(reply->element[i]),
                    parseRedisReply(reply->element[i + 1])
                );
            }
            return map;
        }

        default:
            return Nil{};
    }
}

std::optional<std::string> valueToString(const Value &value) {
    if (std::holds_alternative<String>(value)) {
        return std::get<String>(value);
    }
    if (std::holds_alternative<Nil>(value)) {
        return std::nullopt;
    }
    if (std::holds_alternative<Integer>(value)) {
        return std::to_string(std::get<Integer>(value));
    }
    return std::nullopt;
}

std::int64_t valueToInt(const Value &value, std::int64_t defaultVal = 0) {
    if (std::holds_alternative<Integer>(value)) {
        return std::get<Integer>(value);
    }
    if (std::holds_alternative<String>(value)) {
        const auto &str = std::get<String>(value);
        std::int64_t result = defaultVal;
        std::from_chars(str.data(), str.data() + str.size(), result);
        return result;
    }
    return defaultVal;
}

bool valueToBool(const Value &value) {
    if (std::holds_alternative<Boolean>(value)) {
        return std::get<Boolean>(value);
    }
    if (std::holds_alternative<Integer>(value)) {
        return std::get<Integer>(value) != 0;
    }
    return false;
}

std::vector<std::string> valueToStringVector(const Value &value) {
    std::vector<std::string> result;
    if (auto *arr = std::get_if<std::unique_ptr<Array>>(&value)) {
        result.reserve((*arr)->elements.size());
        for (const auto &elem : (*arr)->elements) {
            if (auto str = valueToString(elem)) {
                result.push_back(std::move(*str));
            }
        }
    }
    return result;
}

std::vector<OptionalString> valueToOptionalStringVector(const Value &value) {
    std::vector<OptionalString> result;
    if (auto *arr = std::get_if<std::unique_ptr<Array>>(&value)) {
        result.reserve((*arr)->elements.size());
        for (const auto &elem : (*arr)->elements) {
            result.push_back(valueToString(elem));
        }
    }
    return result;
}

}  // namespace

// ==================== Client Implementation ====================

struct CommandContext {
    Promise<Value, std::error_code> promise;
};

struct Client::Impl {
    Config config;
    redisAsyncContext *asyncContext = nullptr;
    bool connected = false;
    std::unique_ptr<sync::Mutex> mutex;

    Impl() : mutex(std::make_unique<sync::Mutex>()) {}

    ~Impl() {
        if (asyncContext) {
            redisAsyncDisconnect(asyncContext);
        }
    }

    static void connectCallback(const redisAsyncContext *c, int status) {
        auto *impl = static_cast<Impl *>(c->data);
        impl->connected = (status == REDIS_OK);
    }

    static void disconnectCallback(const redisAsyncContext *c, int status) {
        auto *impl = static_cast<Impl *>(c->data);
        impl->connected = false;
        // Important: hiredis frees the context after this callback
        // Set to nullptr to prevent double-free in destructor
        impl->asyncContext = nullptr;
    }

    static void commandCallback(redisAsyncContext *c, void *r, void *privdata) {
        auto *ctx = static_cast<CommandContext *>(privdata);
        auto *reply = static_cast<redisReply *>(r);

        if (!reply) {
            ctx->promise.reject(make_error_code(Client::Error::CONNECTION_LOST));
        } else if (reply->type == REDIS_REPLY_ERROR) {
            ctx->promise.reject(make_error_code(Client::Error::COMMAND_FAILED));
        } else {
            ctx->promise.resolve(parseRedisReply(reply));
        }

        delete ctx;
    }

    task::Task<Value, std::error_code> executeCommand(std::vector<std::string> args) {
        if (!connected || !asyncContext) {
            co_return std::unexpected(make_error_code(Error::NOT_CONNECTED));
        }

        std::vector<const char *> argv;
        std::vector<size_t> argvlen;
        argv.reserve(args.size());
        argvlen.reserve(args.size());

        for (const auto &arg : args) {
            argv.push_back(arg.c_str());
            argvlen.push_back(arg.size());
        }

        auto *ctx = new CommandContext();
        auto future = ctx->promise.getFuture();

        int status = redisAsyncCommandArgv(
            asyncContext,
            commandCallback,
            ctx,
            static_cast<int>(args.size()),
            argv.data(),
            argvlen.data()
        );

        if (status != REDIS_OK) {
            delete ctx;
            co_return std::unexpected(make_error_code(Error::COMMAND_FAILED));
        }

        auto result = co_await std::move(future);
        co_return std::move(result);
    }
};

// ==================== Client Public Methods ====================

Client::Client() : mImpl(std::make_unique<Impl>()) {}
Client::~Client() = default;

Client::Client(Client &&) noexcept = default;
Client &Client::operator=(Client &&) noexcept = default;

bool Client::isConnected() const {
    return mImpl && mImpl->connected;
}

task::Task<Client, std::error_code> Client::connect(Config config) {
    Client client;
    client.mImpl->config = std::move(config);

    // Create async context
    redisOptions options = {0};
    REDIS_OPTIONS_SET_TCP(&options,
        client.mImpl->config.host.c_str(),
        client.mImpl->config.port);

    if (client.mImpl->config.connectTimeout.count() > 0) {
        timeval tv;
        auto ms = client.mImpl->config.connectTimeout.count();
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(ms / 1000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>((ms % 1000) * 1000);
        options.connect_timeout = &tv;
    }

    client.mImpl->asyncContext = redisAsyncConnectWithOptions(&options);

    if (!client.mImpl->asyncContext) {
        co_return std::unexpected(make_error_code(Client::Error::CONNECTION_FAILED));
    }

    if (client.mImpl->asyncContext->err) {
        redisAsyncFree(client.mImpl->asyncContext);
        client.mImpl->asyncContext = nullptr;
        co_return std::unexpected(make_error_code(Client::Error::CONNECTION_FAILED));
    }

    client.mImpl->asyncContext->data = client.mImpl.get();

    // Attach to libuv event loop
    if (redisLibuvAttach(client.mImpl->asyncContext, getEventLoop()->raw()) != REDIS_OK) {
        redisAsyncFree(client.mImpl->asyncContext);
        client.mImpl->asyncContext = nullptr;
        co_return std::unexpected(make_error_code(Client::Error::CONNECTION_FAILED));
    }

    redisAsyncSetConnectCallback(client.mImpl->asyncContext, Impl::connectCallback);
    redisAsyncSetDisconnectCallback(client.mImpl->asyncContext, Impl::disconnectCallback);

    // Wait for connection
    // Use a timer to check connection status
    auto startTime = std::chrono::steady_clock::now();
    auto timeout = client.mImpl->config.connectTimeout;

    while (!client.mImpl->connected) {
        co_await sleep(std::chrono::milliseconds{10});

        if (client.mImpl->asyncContext->err) {
            co_return std::unexpected(make_error_code(Client::Error::CONNECTION_FAILED));
        }

        if (timeout.count() > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime);
            if (elapsed >= timeout) {
                co_return std::unexpected(make_error_code(Client::Error::TIMEOUT));
            }
        }
    }

    // Authenticate if needed
    if (!client.mImpl->config.password.empty()) {
        std::vector<std::string> authArgs;
        if (!client.mImpl->config.username.empty()) {
            authArgs = {"AUTH", client.mImpl->config.username, client.mImpl->config.password};
        } else {
            authArgs = {"AUTH", client.mImpl->config.password};
        }

        auto authResult = co_await client.mImpl->executeCommand(std::move(authArgs));
        if (!authResult) {
            co_return std::unexpected(make_error_code(Client::Error::AUTHENTICATION_FAILED));
        }
    }

    // Select database if not default
    if (client.mImpl->config.database != 0) {
        auto selectResult = co_await client.mImpl->executeCommand(
            {"SELECT", std::to_string(client.mImpl->config.database)});
        if (!selectResult) {
            co_return std::unexpected(selectResult.error());
        }
    }

    co_return std::move(client);
}

task::Task<Client, std::error_code> Client::connect(Config config, TLSConfig tlsConfig) {
    // TODO: Implement TLS support with hiredis SSL
    co_return std::unexpected(make_error_code(Client::Error::CONNECTION_FAILED));
}

task::Task<Client, std::error_code> Client::connect(std::string url) {
    auto urlResult = URL::parse(url);
    if (!urlResult) {
        co_return std::unexpected(urlResult.error());
    }

    Config config;
    config.host = urlResult->host;
    config.port = urlResult->port;
    config.username = urlResult->username;
    config.password = urlResult->password;
    config.database = urlResult->database;

    if (urlResult->scheme == "rediss") {
        TLSConfig tlsConfig;
        co_return co_await connect(std::move(config), std::move(tlsConfig));
    }

    co_return co_await connect(std::move(config));
}

task::Task<void, std::error_code> Client::disconnect() {
    if (mImpl->asyncContext) {
        redisAsyncDisconnect(mImpl->asyncContext);
        mImpl->asyncContext = nullptr;
        mImpl->connected = false;
    }
    co_return {};
}

// ==================== Generic Commands ====================

task::Task<Value, std::error_code> Client::command(std::vector<std::string> args) {
    co_return co_await mImpl->executeCommand(std::move(args));
}

task::Task<OptionalString, std::error_code> Client::commandString(std::vector<std::string> args) {
    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToString(*result);
}

task::Task<std::int64_t, std::error_code> Client::commandInt(std::vector<std::string> args) {
    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

// ==================== String Commands ====================

task::Task<OptionalString, std::error_code> Client::get(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"GET", std::string(key)});
    CO_EXPECT(result);
    co_return valueToString(*result);
}

task::Task<bool, std::error_code> Client::set(
    std::string_view key,
    std::string_view value,
    std::chrono::milliseconds ttl,
    bool nx,
    bool xx
) {
    std::vector<std::string> args = {"SET", std::string(key), std::string(value)};

    if (ttl.count() > 0) {
        args.push_back("PX");
        args.push_back(std::to_string(ttl.count()));
    }

    if (nx) {
        args.push_back("NX");
    } else if (xx) {
        args.push_back("XX");
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);

    // SET returns OK on success, nil on NX/XX failure
    if (std::holds_alternative<Nil>(*result)) {
        co_return false;
    }
    co_return true;
}

task::Task<void, std::error_code> Client::setex(
    std::string_view key,
    std::chrono::seconds ttl,
    std::string_view value
) {
    auto result = co_await mImpl->executeCommand({
        "SETEX",
        std::string(key),
        std::to_string(ttl.count()),
        std::string(value)
    });
    CO_EXPECT(result);
    co_return {};
}

task::Task<bool, std::error_code> Client::setnx(std::string_view key, std::string_view value) {
    auto result = co_await mImpl->executeCommand({"SETNX", std::string(key), std::string(value)});
    CO_EXPECT(result);
    co_return valueToInt(*result) == 1;
}

task::Task<std::vector<OptionalString>, std::error_code>
Client::mget(std::span<const std::string> keys) {
    std::vector<std::string> args = {"MGET"};
    args.reserve(keys.size() + 1);
    for (const auto &key : keys) {
        args.push_back(key);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToOptionalStringVector(*result);
}

task::Task<void, std::error_code>
Client::mset(std::span<const std::pair<std::string, std::string>> kvs) {
    std::vector<std::string> args = {"MSET"};
    args.reserve(kvs.size() * 2 + 1);
    for (const auto &[key, value] : kvs) {
        args.push_back(key);
        args.push_back(value);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return {};
}

task::Task<std::int64_t, std::error_code> Client::incr(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"INCR", std::string(key)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::int64_t, std::error_code> Client::incrby(std::string_view key, std::int64_t delta) {
    auto result = co_await mImpl->executeCommand({"INCRBY", std::string(key), std::to_string(delta)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::int64_t, std::error_code> Client::decr(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"DECR", std::string(key)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::int64_t, std::error_code> Client::append(std::string_view key, std::string_view value) {
    auto result = co_await mImpl->executeCommand({"APPEND", std::string(key), std::string(value)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

// ==================== Key Commands ====================

task::Task<std::int64_t, std::error_code> Client::del(std::span<const std::string> keys) {
    std::vector<std::string> args = {"DEL"};
    args.reserve(keys.size() + 1);
    for (const auto &key : keys) {
        args.push_back(key);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<bool, std::error_code> Client::del(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"DEL", std::string(key)});
    CO_EXPECT(result);
    co_return valueToInt(*result) == 1;
}

task::Task<std::int64_t, std::error_code> Client::exists(std::span<const std::string> keys) {
    std::vector<std::string> args = {"EXISTS"};
    args.reserve(keys.size() + 1);
    for (const auto &key : keys) {
        args.push_back(key);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<bool, std::error_code> Client::exists(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"EXISTS", std::string(key)});
    CO_EXPECT(result);
    co_return valueToInt(*result) == 1;
}

task::Task<bool, std::error_code> Client::expire(std::string_view key, std::chrono::seconds seconds) {
    auto result = co_await mImpl->executeCommand({"EXPIRE", std::string(key), std::to_string(seconds.count())});
    CO_EXPECT(result);
    co_return valueToInt(*result) == 1;
}

task::Task<bool, std::error_code> Client::pexpire(std::string_view key, std::chrono::milliseconds ms) {
    auto result = co_await mImpl->executeCommand({"PEXPIRE", std::string(key), std::to_string(ms.count())});
    CO_EXPECT(result);
    co_return valueToInt(*result) == 1;
}

task::Task<std::int64_t, std::error_code> Client::ttl(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"TTL", std::string(key)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::int64_t, std::error_code> Client::pttl(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"PTTL", std::string(key)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::vector<std::string>, std::error_code> Client::keys(std::string_view pattern) {
    auto result = co_await mImpl->executeCommand({"KEYS", std::string(pattern)});
    CO_EXPECT(result);
    co_return valueToStringVector(*result);
}

task::Task<ScanResult<std::string>, std::error_code>
Client::scan(std::string_view cursor, std::string_view pattern, std::size_t count) {
    auto result = co_await mImpl->executeCommand({
        "SCAN", std::string(cursor),
        "MATCH", std::string(pattern),
        "COUNT", std::to_string(count)
    });
    CO_EXPECT(result);

    ScanResult<std::string> scanResult;
    if (auto *arr = std::get_if<std::unique_ptr<Array>>(&*result)) {
        if ((*arr)->elements.size() >= 2) {
            scanResult.cursor = valueToString((*arr)->elements[0]).value_or("0");
            scanResult.keys = valueToStringVector((*arr)->elements[1]);
        }
    }

    co_return scanResult;
}

task::Task<std::string, std::error_code> Client::type(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"TYPE", std::string(key)});
    CO_EXPECT(result);
    co_return valueToString(*result).value_or("none");
}

task::Task<void, std::error_code> Client::rename(std::string_view key, std::string_view newKey) {
    auto result = co_await mImpl->executeCommand({"RENAME", std::string(key), std::string(newKey)});
    CO_EXPECT(result);
    co_return {};
}

// ==================== Hash Commands ====================

task::Task<OptionalString, std::error_code> Client::hget(std::string_view key, std::string_view field) {
    auto result = co_await mImpl->executeCommand({"HGET", std::string(key), std::string(field)});
    CO_EXPECT(result);
    co_return valueToString(*result);
}

task::Task<std::int64_t, std::error_code>
Client::hset(std::string_view key, std::span<const std::pair<std::string, std::string>> fieldValues) {
    std::vector<std::string> args = {"HSET", std::string(key)};
    args.reserve(fieldValues.size() * 2 + 2);
    for (const auto &[field, value] : fieldValues) {
        args.push_back(field);
        args.push_back(value);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<bool, std::error_code>
Client::hset(std::string_view key, std::string_view field, std::string_view value) {
    auto result = co_await mImpl->executeCommand({"HSET", std::string(key), std::string(field), std::string(value)});
    CO_EXPECT(result);
    co_return valueToInt(*result) == 1;
}

task::Task<std::vector<OptionalString>, std::error_code>
Client::hmget(std::string_view key, std::span<const std::string> fields) {
    std::vector<std::string> args = {"HMGET", std::string(key)};
    args.reserve(fields.size() + 2);
    for (const auto &field : fields) {
        args.push_back(field);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToOptionalStringVector(*result);
}

task::Task<std::vector<std::pair<std::string, std::string>>, std::error_code>
Client::hgetall(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"HGETALL", std::string(key)});
    CO_EXPECT(result);

    std::vector<std::pair<std::string, std::string>> pairs;
    if (auto *arr = std::get_if<std::unique_ptr<Array>>(&*result)) {
        pairs.reserve((*arr)->elements.size() / 2);
        for (size_t i = 0; i + 1 < (*arr)->elements.size(); i += 2) {
            auto field = valueToString((*arr)->elements[i]);
            auto value = valueToString((*arr)->elements[i + 1]);
            if (field && value) {
                pairs.emplace_back(std::move(*field), std::move(*value));
            }
        }
    }

    co_return pairs;
}

task::Task<std::int64_t, std::error_code>
Client::hdel(std::string_view key, std::span<const std::string> fields) {
    std::vector<std::string> args = {"HDEL", std::string(key)};
    args.reserve(fields.size() + 2);
    for (const auto &field : fields) {
        args.push_back(field);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<bool, std::error_code> Client::hexists(std::string_view key, std::string_view field) {
    auto result = co_await mImpl->executeCommand({"HEXISTS", std::string(key), std::string(field)});
    CO_EXPECT(result);
    co_return valueToInt(*result) == 1;
}

task::Task<std::int64_t, std::error_code> Client::hlen(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"HLEN", std::string(key)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::int64_t, std::error_code>
Client::hincrby(std::string_view key, std::string_view field, std::int64_t delta) {
    auto result = co_await mImpl->executeCommand({"HINCRBY", std::string(key), std::string(field), std::to_string(delta)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

// ==================== List Commands ====================

task::Task<std::int64_t, std::error_code>
Client::lpush(std::string_view key, std::span<const std::string> elements) {
    std::vector<std::string> args = {"LPUSH", std::string(key)};
    args.reserve(elements.size() + 2);
    for (const auto &elem : elements) {
        args.push_back(elem);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::int64_t, std::error_code>
Client::rpush(std::string_view key, std::span<const std::string> elements) {
    std::vector<std::string> args = {"RPUSH", std::string(key)};
    args.reserve(elements.size() + 2);
    for (const auto &elem : elements) {
        args.push_back(elem);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<OptionalString, std::error_code> Client::lpop(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"LPOP", std::string(key)});
    CO_EXPECT(result);
    co_return valueToString(*result);
}

task::Task<OptionalString, std::error_code> Client::rpop(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"RPOP", std::string(key)});
    CO_EXPECT(result);
    co_return valueToString(*result);
}

task::Task<std::vector<std::string>, std::error_code>
Client::lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    auto result = co_await mImpl->executeCommand({"LRANGE", std::string(key), std::to_string(start), std::to_string(stop)});
    CO_EXPECT(result);
    co_return valueToStringVector(*result);
}

task::Task<std::int64_t, std::error_code> Client::llen(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"LLEN", std::string(key)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::optional<std::pair<std::string, std::string>>, std::error_code>
Client::blpop(std::span<const std::string> keys, std::chrono::seconds timeout) {
    std::vector<std::string> args = {"BLPOP"};
    args.reserve(keys.size() + 2);
    for (const auto &key : keys) {
        args.push_back(key);
    }
    args.push_back(std::to_string(timeout.count()));

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);

    if (std::holds_alternative<Nil>(*result)) {
        co_return std::nullopt;
    }

    if (auto *arr = std::get_if<std::unique_ptr<Array>>(&*result)) {
        if ((*arr)->elements.size() >= 2) {
            auto key = valueToString((*arr)->elements[0]);
            auto value = valueToString((*arr)->elements[1]);
            if (key && value) {
                co_return std::make_pair(std::move(*key), std::move(*value));
            }
        }
    }

    co_return std::nullopt;
}

task::Task<std::optional<std::pair<std::string, std::string>>, std::error_code>
Client::brpop(std::span<const std::string> keys, std::chrono::seconds timeout) {
    std::vector<std::string> args = {"BRPOP"};
    args.reserve(keys.size() + 2);
    for (const auto &key : keys) {
        args.push_back(key);
    }
    args.push_back(std::to_string(timeout.count()));

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);

    if (std::holds_alternative<Nil>(*result)) {
        co_return std::nullopt;
    }

    if (auto *arr = std::get_if<std::unique_ptr<Array>>(&*result)) {
        if ((*arr)->elements.size() >= 2) {
            auto key = valueToString((*arr)->elements[0]);
            auto value = valueToString((*arr)->elements[1]);
            if (key && value) {
                co_return std::make_pair(std::move(*key), std::move(*value));
            }
        }
    }

    co_return std::nullopt;
}

// ==================== Set Commands ====================

task::Task<std::int64_t, std::error_code>
Client::sadd(std::string_view key, std::span<const std::string> members) {
    std::vector<std::string> args = {"SADD", std::string(key)};
    args.reserve(members.size() + 2);
    for (const auto &member : members) {
        args.push_back(member);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::int64_t, std::error_code>
Client::srem(std::string_view key, std::span<const std::string> members) {
    std::vector<std::string> args = {"SREM", std::string(key)};
    args.reserve(members.size() + 2);
    for (const auto &member : members) {
        args.push_back(member);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::vector<std::string>, std::error_code> Client::smembers(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"SMEMBERS", std::string(key)});
    CO_EXPECT(result);
    co_return valueToStringVector(*result);
}

task::Task<bool, std::error_code> Client::sismember(std::string_view key, std::string_view member) {
    auto result = co_await mImpl->executeCommand({"SISMEMBER", std::string(key), std::string(member)});
    CO_EXPECT(result);
    co_return valueToInt(*result) == 1;
}

task::Task<std::int64_t, std::error_code> Client::scard(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"SCARD", std::string(key)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

// ==================== Sorted Set Commands ====================

task::Task<std::int64_t, std::error_code>
Client::zadd(std::string_view key, std::span<const std::pair<double, std::string>> scoreMembers,
             bool nx, bool xx, bool ch) {
    std::vector<std::string> args = {"ZADD", std::string(key)};

    if (nx) args.push_back("NX");
    else if (xx) args.push_back("XX");

    if (ch) args.push_back("CH");

    args.reserve(args.size() + scoreMembers.size() * 2);
    for (const auto &[score, member] : scoreMembers) {
        args.push_back(std::to_string(score));
        args.push_back(member);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::vector<std::string>, std::error_code>
Client::zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    auto result = co_await mImpl->executeCommand({"ZRANGE", std::string(key), std::to_string(start), std::to_string(stop)});
    CO_EXPECT(result);
    co_return valueToStringVector(*result);
}

task::Task<std::vector<std::pair<std::string, double>>, std::error_code>
Client::zrangeWithScores(std::string_view key, std::int64_t start, std::int64_t stop) {
    auto result = co_await mImpl->executeCommand({"ZRANGE", std::string(key), std::to_string(start), std::to_string(stop), "WITHSCORES"});
    CO_EXPECT(result);

    std::vector<std::pair<std::string, double>> pairs;
    if (auto *arr = std::get_if<std::unique_ptr<Array>>(&*result)) {
        pairs.reserve((*arr)->elements.size() / 2);
        for (size_t i = 0; i + 1 < (*arr)->elements.size(); i += 2) {
            auto member = valueToString((*arr)->elements[i]);
            auto scoreStr = valueToString((*arr)->elements[i + 1]);
            if (member && scoreStr) {
                double score = std::stod(*scoreStr);
                pairs.emplace_back(std::move(*member), score);
            }
        }
    }

    co_return pairs;
}

task::Task<std::optional<std::int64_t>, std::error_code>
Client::zrank(std::string_view key, std::string_view member) {
    auto result = co_await mImpl->executeCommand({"ZRANK", std::string(key), std::string(member)});
    CO_EXPECT(result);

    if (std::holds_alternative<Nil>(*result)) {
        co_return std::nullopt;
    }
    co_return valueToInt(*result);
}

task::Task<std::optional<double>, std::error_code>
Client::zscore(std::string_view key, std::string_view member) {
    auto result = co_await mImpl->executeCommand({"ZSCORE", std::string(key), std::string(member)});
    CO_EXPECT(result);

    if (std::holds_alternative<Nil>(*result)) {
        co_return std::nullopt;
    }

    if (auto str = valueToString(*result)) {
        co_return std::stod(*str);
    }

    if (std::holds_alternative<Double>(*result)) {
        co_return std::get<Double>(*result);
    }

    co_return std::nullopt;
}

task::Task<std::int64_t, std::error_code> Client::zcard(std::string_view key) {
    auto result = co_await mImpl->executeCommand({"ZCARD", std::string(key)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<std::int64_t, std::error_code>
Client::zrem(std::string_view key, std::span<const std::string> members) {
    std::vector<std::string> args = {"ZREM", std::string(key)};
    args.reserve(members.size() + 2);
    for (const auto &member : members) {
        args.push_back(member);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

// ==================== Pub/Sub Commands ====================

task::Task<std::int64_t, std::error_code>
Client::publish(std::string_view channel, std::string_view message) {
    auto result = co_await mImpl->executeCommand({"PUBLISH", std::string(channel), std::string(message)});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<void, std::error_code> Client::subscribe(std::span<const std::string> channels) {
    std::vector<std::string> args = {"SUBSCRIBE"};
    args.reserve(channels.size() + 1);
    for (const auto &ch : channels) {
        args.push_back(ch);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return {};
}

task::Task<void, std::error_code> Client::unsubscribe(std::span<const std::string> channels) {
    std::vector<std::string> args = {"UNSUBSCRIBE"};
    for (const auto &ch : channels) {
        args.push_back(ch);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return {};
}

task::Task<void, std::error_code> Client::psubscribe(std::span<const std::string> patterns) {
    std::vector<std::string> args = {"PSUBSCRIBE"};
    args.reserve(patterns.size() + 1);
    for (const auto &pattern : patterns) {
        args.push_back(pattern);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return {};
}

task::Task<void, std::error_code> Client::punsubscribe(std::span<const std::string> patterns) {
    std::vector<std::string> args = {"PUNSUBSCRIBE"};
    for (const auto &pattern : patterns) {
        args.push_back(pattern);
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return {};
}

task::Task<PubSubMessage, std::error_code> Client::receivePubSub() {
    // TODO: Implement pub/sub message receiving
    co_return std::unexpected(make_error_code(Error::NOT_CONNECTED));
}

// ==================== Server Commands ====================

task::Task<std::string, std::error_code> Client::ping(std::string_view message) {
    std::vector<std::string> args = {"PING"};
    if (!message.empty()) {
        args.push_back(std::string(message));
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToString(*result).value_or("PONG");
}

task::Task<std::string, std::error_code> Client::echo(std::string_view message) {
    auto result = co_await mImpl->executeCommand({"ECHO", std::string(message)});
    CO_EXPECT(result);
    co_return valueToString(*result).value_or("");
}

task::Task<std::string, std::error_code> Client::info(std::string_view section) {
    std::vector<std::string> args = {"INFO"};
    if (!section.empty()) {
        args.push_back(std::string(section));
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return valueToString(*result).value_or("");
}

task::Task<std::int64_t, std::error_code> Client::dbsize() {
    auto result = co_await mImpl->executeCommand({"DBSIZE"});
    CO_EXPECT(result);
    co_return valueToInt(*result);
}

task::Task<void, std::error_code> Client::flushdb(bool async) {
    std::vector<std::string> args = {"FLUSHDB"};
    if (async) {
        args.push_back("ASYNC");
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return {};
}

task::Task<void, std::error_code> Client::flushall(bool async) {
    std::vector<std::string> args = {"FLUSHALL"};
    if (async) {
        args.push_back("ASYNC");
    }

    auto result = co_await mImpl->executeCommand(std::move(args));
    CO_EXPECT(result);
    co_return {};
}

task::Task<void, std::error_code> Client::select(int database) {
    auto result = co_await mImpl->executeCommand({"SELECT", std::to_string(database)});
    CO_EXPECT(result);
    co_return {};
}

}  // namespace asyncio::redis
