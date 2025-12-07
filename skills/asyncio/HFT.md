# asyncio HFT & Low-Latency Guide

## Latency Profile

asyncio achieves **sub-microsecond per-message latency**:

| Metric | asyncio | Notes |
|--------|---------|-------|
| TCP per-message | **0.046μs** | Best-in-class |
| Request/response | **27M msg/s** | 2x faster than alternatives |
| WebSocket throughput | **2.2 GiB/s** | After optimization |

## Trading System Suitability

| System Type | Recommendation |
|-------------|---------------|
| Market making (<1μs) | Not recommended (need kernel bypass) |
| Statistical arbitrage | Recommended |
| Smart order routing | **Highly recommended** |
| Algorithmic trading | **Highly recommended** |
| Risk management | **Highly recommended** |

## Network Optimizations

### 1. TCP_NODELAY (Critical)

Always disable Nagle's algorithm for trading:

```cpp
asyncio::task::Task<void, std::error_code> handleOrder(asyncio::net::TCPStream stream) {
    // MUST be first operation after connect/accept
    stream.noDelay(true);

    // Now handle orders...
}
```

### 2. io_uring (Linux)

Enable for reduced syscall overhead:

```bash
# Production launch
UV_USE_IO_URING=1 taskset -c 2-3 ./trading_app
```

**Requirements:**
- Linux kernel 5.10.186+
- Not available on 32-bit ARM, ppc64

**Benefits:**
- Up to 8x throughput for file I/O
- Batched syscalls
- Reduced context switches

### 3. CPU Pinning

```cpp
#include <pthread.h>
#include <sched.h>

void pinToCore(int coreId) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

// Pin trading thread to isolated core
pinToCore(2);
```

## Memory Optimizations

### Pre-allocated Buffers

Avoid allocation in the hot path:

```cpp
// Thread-local buffer for frame operations
constexpr std::size_t MAX_MESSAGE_SIZE = 64 * 1024;
thread_local std::vector<std::byte> g_sendBuffer(MAX_MESSAGE_SIZE);
thread_local std::vector<std::byte> g_recvBuffer(MAX_MESSAGE_SIZE);

asyncio::task::Task<void, std::error_code> sendOrder(
    asyncio::net::TCPStream& stream,
    const Order& order
) {
    // Serialize directly into pre-allocated buffer
    std::size_t len = order.serialize(g_sendBuffer.data());
    CO_EXPECT(co_await stream.writeAll({g_sendBuffer.data(), len}));
    co_return {};
}
```

### Single-Write Operations

Combine header and payload to reduce syscalls:

```cpp
asyncio::task::Task<void, std::error_code> sendFrame(
    asyncio::IWriter& writer,
    std::span<const std::byte> header,
    std::span<const std::byte> payload
) {
    const std::size_t totalSize = header.size() + payload.size();

    // Single allocation + single write
    std::memcpy(g_sendBuffer.data(), header.data(), header.size());
    std::memcpy(g_sendBuffer.data() + header.size(), payload.data(), payload.size());

    CO_EXPECT(co_await writer.writeAll({g_sendBuffer.data(), totalSize}));
    co_return {};
}
```

### Vectorized Operations

For XOR operations (WebSocket unmasking), process 8 bytes at a time:

```cpp
inline void unmaskData(std::span<std::byte> data, const std::array<std::byte, 4>& maskKey) {
    // Create 8-byte mask
    std::uint64_t mask64;
    std::memcpy(&mask64, maskKey.data(), 4);
    mask64 |= (mask64 << 32);

    // Process 8 bytes at a time
    auto* ptr = reinterpret_cast<std::uint64_t*>(data.data());
    const std::size_t chunks = data.size() / 8;
    for (std::size_t c = 0; c < chunks; ++c, ++ptr) {
        *ptr ^= mask64;
    }

    // Handle remaining bytes
    for (std::size_t i = chunks * 8; i < data.size(); ++i) {
        data[i] ^= maskKey[i % 4];
    }
}
```

## Order Routing Patterns

### Timeout Handling

```cpp
asyncio::task::Task<OrderResult, std::error_code> executeOrder(Order order) {
    auto orderTask = sendToExchange(order);

    // Cancel if no response in 10ms
    auto result = co_await asyncio::race(orderTask, asyncio::sleep(10ms));

    if (result.index() == 1) {
        orderTask.cancel();
        co_return OrderResult::TIMEOUT;
    }

    co_return std::get<0>(result);
}
```

### Multi-Venue Routing

```cpp
asyncio::task::Task<Fill, std::error_code> routeOrder(Order order) {
    // Send to first responding venue
    auto fill = co_await asyncio::any(
        sendToVenue(order, "NYSE"),
        sendToVenue(order, "NASDAQ"),
        sendToVenue(order, "BATS"),
        sendToVenue(order, "IEX")
    );

    co_return fill;
}
```

### Best Price Selection

```cpp
asyncio::task::Task<Venue, std::error_code> selectBestVenue(Symbol symbol) {
    // Get quotes from all venues in parallel
    auto [nyse, nasdaq, bats] = co_await asyncio::all(
        getQuote(symbol, "NYSE"),
        getQuote(symbol, "NASDAQ"),
        getQuote(symbol, "BATS")
    );

    // Select best price
    return selectBestPrice(nyse, nasdaq, bats);
}
```

## Market Data Distribution

Use channels for distributing market data to multiple strategies:

```cpp
// Market data feed handler
asyncio::task::Task<void, std::error_code> marketDataFeed(
    asyncio::Channel<MarketData>::Sender sender
) {
    while (true) {
        auto tick = co_await readNextTick();
        CO_EXPECT(tick);
        co_await sender.send(*tick);
    }
}

// Strategy consumer
asyncio::task::Task<void, std::error_code> strategy(
    asyncio::Channel<MarketData>::Receiver receiver
) {
    while (auto tick = co_await receiver.receive()) {
        processSignal(*tick);
    }
    co_return {};
}

// Setup
auto [sender, receiver] = asyncio::Channel<MarketData>::make(10000);
asyncio::all(
    marketDataFeed(std::move(sender)),
    strategy(std::move(receiver))
);
```

## Graceful Shutdown

```cpp
asyncio::task::Task<void, std::error_code> tradingServer() {
    auto listener = asyncio::net::TCPListener::listen("0.0.0.0", 9000);
    CO_EXPECT(listener);

    auto sigHandler = asyncio::signal::on(SIGINT);

    // Race between serving and shutdown signal
    co_await asyncio::race(
        serveConnections(*listener),
        sigHandler
    );

    // Graceful shutdown
    co_await listener->close();
    co_return {};
}
```

## Monitoring & Metrics

```cpp
#include <chrono>

struct LatencyMetrics {
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> totalNs{0};
    std::atomic<std::uint64_t> maxNs{0};

    void record(std::chrono::nanoseconds latency) {
        count.fetch_add(1, std::memory_order_relaxed);
        totalNs.fetch_add(latency.count(), std::memory_order_relaxed);

        std::uint64_t current = maxNs.load(std::memory_order_relaxed);
        while (latency.count() > current &&
               !maxNs.compare_exchange_weak(current, latency.count()));
    }
};

thread_local LatencyMetrics g_orderLatency;

asyncio::task::Task<OrderResult, std::error_code> sendOrder(Order order) {
    auto start = std::chrono::steady_clock::now();

    auto result = co_await executeOrder(order);

    auto end = std::chrono::steady_clock::now();
    g_orderLatency.record(end - start);

    co_return result;
}
```

## Comparison with Alternatives

### asyncio vs Aeron

| Aspect | asyncio | Aeron |
|--------|---------|-------|
| TCP latency | **0.046μs/msg** | N/A (UDP-based) |
| UDP multicast | Not optimized | **Designed for this** |
| Cluster/HA | Manual | **Built-in** |
| API | Simple coroutines | Callbacks |

**Choose Aeron for:** UDP multicast, cluster consensus, Java/.NET interop

**Choose asyncio for:** TCP request/response, simple API, C++ only

### asyncio vs Boost.Asio

| Aspect | asyncio | Boost.Asio |
|--------|---------|------------|
| Request/response | **27M msg/s** | 124K msg/s |
| Bulk transfer (64KB) | 2.9 GiB/s | **8.0 GiB/s** |
| API complexity | Simple | Complex |
| Coroutine support | Native C++23 | Add-on |

**Choose Asio for:** Large bulk transfers, existing Asio codebase

**Choose asyncio for:** Request/response patterns, clean API, modern C++

## Kernel Bypass (Ultra-HFT)

For latencies <1μs, asyncio is not suitable. Consider:

- **Solarflare/Xilinx OpenOnload** - Kernel bypass networking
- **DPDK** - Direct NIC access
- **Mellanox VMA** - Verbs messaging accelerator
- **Custom FPGA** - Hardware acceleration

asyncio's libuv foundation uses standard socket APIs with kernel transitions.
