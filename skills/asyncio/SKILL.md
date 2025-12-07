---
name: asyncio-cpp
description: Write high-performance, low-latency C++ networking code using the asyncio framework. Use when writing TCP/UDP servers, WebSocket clients, HTTP clients, or any async I/O code in C++. Covers coroutines, task composition, channels, and HFT optimizations.
allowed-tools: Read, Write, Edit, Grep, Glob, Bash
---

# asyncio C++ High-Performance Networking

## Overview

asyncio is a C++23 coroutine-based network framework built on libuv. It provides callback-free async programming using stackless coroutines with `Task<T, E>` as the core abstraction.

**Requirements:** GCC 14+, LLVM 18+, or MSVC 19.38+ with C++23 support.

## Quick Start

### Entry Point

```cpp
#include <asyncio/asyncio.h>

asyncio::task::Task<void, std::error_code> asyncMain(int argc, char* argv[]) {
    // Your async code here
    co_return {};
}
```

Or use `asyncio::run()` directly:

```cpp
int main() {
    auto result = asyncio::run([]() -> asyncio::task::Task<void, std::error_code> {
        co_await asyncio::sleep(1s);
        co_return {};
    });
}
```

## TCP Server (High Performance)

```cpp
#include <asyncio/net/stream.h>

asyncio::task::Task<void, std::error_code> handleClient(asyncio::net::TCPStream stream) {
    // HFT optimization: disable Nagle's algorithm
    stream.noDelay(true);

    std::array<std::byte, 8192> buffer;
    while (true) {
        auto n = co_await stream.read(buffer);
        if (!n || *n == 0) break;
        CO_EXPECT(co_await stream.writeAll({buffer.data(), *n}));
    }
    co_return {};
}

asyncio::task::Task<void, std::error_code> runServer() {
    auto listener = asyncio::net::TCPListener::listen("0.0.0.0", 8080);
    if (!listener) co_return std::unexpected{listener.error()};

    while (true) {
        auto stream = co_await listener->accept();
        if (!stream) break;
        // Fire and forget - handle connection concurrently
        handleClient(*std::move(stream)).future().fail([](auto&) {});
    }
    co_return {};
}
```

## TCP Client

```cpp
asyncio::task::Task<std::string, std::error_code> fetchData(
    const std::string& host,
    std::uint16_t port
) {
    auto stream = co_await asyncio::net::TCPStream::connect(host, port);
    CO_EXPECT(stream);

    // HFT optimization
    stream->noDelay(true);

    std::string request = "GET / HTTP/1.1\r\nHost: " + host + "\r\n\r\n";
    CO_EXPECT(co_await stream->writeAll(std::as_bytes(std::span{request})));

    auto response = co_await stream->readAll();
    CO_EXPECT(response);

    co_return std::string(
        reinterpret_cast<const char*>(response->data()),
        response->size()
    );
}
```

## Error Handling

asyncio supports two error modes:

### Mode 1: Error Codes (Recommended for HFT)

```cpp
asyncio::task::Task<Result, std::error_code> process() {
    auto data = co_await fetchData();
    CO_EXPECT(data);  // Early return on error

    auto result = co_await transform(*data);
    CO_EXPECT(result);

    co_return *result;
}
```

### Mode 2: Exceptions

```cpp
asyncio::task::Task<Result> process() {
    auto data = co_await fetchData();  // Throws on error
    auto result = co_await transform(data);
    co_return result;
}
```

## Task Composition

### Wait for All (Cancel on First Failure)

```cpp
auto [user, profile] = co_await asyncio::all(
    fetchUser(userId),
    fetchProfile(userId)
);
```

### Race (First Completion Wins)

```cpp
// Timeout pattern
auto result = co_await asyncio::race(
    fetchData(url),
    asyncio::sleep(30s)
);
```

### Any (First Success Wins)

```cpp
// Multi-venue order routing
auto fill = co_await asyncio::any(
    sendToVenue(order, "NYSE"),
    sendToVenue(order, "NASDAQ"),
    sendToVenue(order, "BATS")
);
```

### TaskGroup for Dynamic Tasks

```cpp
asyncio::task::TaskGroup group;
while (auto conn = co_await listener->accept()) {
    auto task = handleConnection(*std::move(conn));
    group.add(task);
}
co_await group;  // Wait for all handlers
```

## Cancellation

```cpp
auto longTask = computeForever();

// Later, when shutdown requested:
if (shutdownRequested) {
    longTask.cancel();  // Propagates through task tree
    co_await longTask;  // Completes with CANCELLED error
}
```

## Channel Communication (Go-style)

```cpp
auto [sender, receiver] = asyncio::Channel<Message>::make(100);

// Producer
co_await sender.send(message);

// Consumer
while (auto msg = co_await receiver.receive()) {
    process(*msg);
}
```

## Async Synchronization Primitives

```cpp
asyncio::sync::Mutex mutex;
asyncio::sync::Event event;
asyncio::sync::Condition condition;

// Async lock
{
    auto guard = co_await mutex.lock();
    // Critical section
}  // Auto-unlock

// Wait for event
co_await event.wait();

// Condition variable
co_await condition.wait(lock, [] { return ready; });
```

## HFT Optimizations

For detailed HFT guidance, see [HFT.md](HFT.md).

### Key Optimizations

1. **TCP_NODELAY** - Always enable for low-latency:
   ```cpp
   stream.noDelay(true);
   ```

2. **io_uring** (Linux) - Enable via environment:
   ```bash
   UV_USE_IO_URING=1 ./trading_app
   ```

3. **Pre-allocated Buffers** - Avoid allocation in hot path:
   ```cpp
   thread_local std::vector<std::byte> buffer(64 * 1024);
   ```

4. **Single-write Operations** - Combine header + payload:
   ```cpp
   // Bad: multiple syscalls
   co_await stream.writeAll(header);
   co_await stream.writeAll(payload);

   // Good: single syscall
   std::memcpy(buffer.data(), header.data(), header.size());
   std::memcpy(buffer.data() + header.size(), payload.data(), payload.size());
   co_await stream.writeAll({buffer.data(), header.size() + payload.size()});
   ```

## WebSocket Client

```cpp
#include <asyncio/http/websocket.h>

asyncio::task::Task<void, std::error_code> wsClient() {
    auto ws = co_await asyncio::http::ws::WebSocket::connect(
        *asyncio::http::URL::from("ws://localhost:8080/")
    );
    CO_EXPECT(ws);

    CO_EXPECT(co_await ws->sendText("Hello"));

    auto msg = co_await ws->readMessage();
    CO_EXPECT(msg);

    co_await ws->close(asyncio::http::ws::CloseCode::NORMAL_CLOSURE);
    co_return {};
}
```

## HTTP Client

```cpp
#include <asyncio/http/request.h>

asyncio::task::Task<void, std::error_code> httpClient() {
    auto response = co_await asyncio::http::request(
        *asyncio::http::URL::from("https://api.example.com/data"),
        {
            .method = asyncio::http::Method::GET,
            .headers = {{"Authorization", "Bearer token"}}
        }
    );
    CO_EXPECT(response);

    auto body = co_await response->body.readAll();
    CO_EXPECT(body);

    co_return {};
}
```

## File I/O

```cpp
#include <asyncio/fs.h>

asyncio::task::Task<std::string, std::error_code> readFile(std::string path) {
    auto file = co_await asyncio::fs::open(path, asyncio::fs::OpenMode::READ);
    CO_EXPECT(file);

    auto content = co_await file->readAll();
    CO_EXPECT(content);

    co_return std::string(
        reinterpret_cast<const char*>(content->data()),
        content->size()
    );
}
```

## Performance Benchmarks

| Pattern | asyncio | Asio Async | Trantor |
|---------|---------|------------|---------|
| TCP req/resp (64B x1000) | **27M/s** | 124K/s | 14M/s |
| TCP throughput (64KB) | 2.9 GiB/s | 8.0 GiB/s | - |
| WebSocket (64KB) | 2.2 GiB/s | - | - |

asyncio excels at **request/response patterns** (2-200x faster), while Asio has better bulk transfer for large payloads.

## Best Practices

1. **Always use `CO_EXPECT`** for error propagation in coroutines
2. **Enable `noDelay(true)`** for latency-sensitive applications
3. **Use `TaskGroup`** for managing dynamic concurrent tasks
4. **Prefer `race()` with timeout** over blocking waits
5. **Use channels** for inter-task communication instead of shared state
6. **Pre-allocate buffers** for high-frequency operations
7. **Enable io_uring** on Linux for maximum performance
