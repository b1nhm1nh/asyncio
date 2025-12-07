# Low-Latency Networking

The `asyncio::net::lowlatency` namespace provides high-performance, low-latency networking components for scenarios where every microsecond counts.

## Components

### UDP Transport (`asyncio/net/lowlatency/udp.h`)

Raw UDP transport optimized for minimal latency. Use cases:
- Market data streaming
- Localhost benchmarks
- LAN environments with minimal packet loss

**Performance**: ~1-2us vs ~2ms with KCP ARQ

```cpp
#include <asyncio/net/lowlatency/udp.h>

using namespace asyncio::net::lowlatency;

// Create low-latency UDP transport
auto transport = create_low_latency_transport();
transport.bind(12345);

// Set receive callback
transport.on_receive([](const void* data, size_t len, const Endpoint& sender) {
    // Handle received data
});

// Start receiver thread
transport.start();

// Send data
Endpoint dest{"127.0.0.1", 12346};
transport.send_to(data, len, dest);

// Stop when done
transport.stop();
```

**Features**:
- SO_BUSY_POLL for Linux (saves ~7us)
- IP_TOS LOWDELAY QoS marking
- sendmmsg/recvmmsg batch operations
- Connected UDP for fastest send path
- Zero-copy receive callbacks
- Multicast support

### Shared Memory IPC (`asyncio/net/lowlatency/shm.h`)

Zero-copy shared memory communication with async/await support.

**Patterns**:
- SPSC Queue (Single Producer, Single Consumer)
- SPMC Queue (Pub/Sub)
- MPSC Queue (Multiple Producers)
- RPC Channel (Request/Response)
- Memory Pool with loan/borrow API

```cpp
#include <asyncio/net/lowlatency/shm.h>

using namespace asyncio::net::lowlatency::shm;

// Message type (must be trivially copyable)
struct alignas(64) MarketData {
    uint64_t sequence;
    double price;
    uint64_t volume;
};

// Producer
auto queue = AsyncSPSCQueue<MarketData>::create("my-queue", 1024);
queue.try_send(MarketData{1, 50000.0, 100});

// Consumer
auto queue = AsyncSPSCQueue<MarketData>::open("my-queue");
auto msg = queue.try_receive();
if (msg) {
    // Process *msg
}

// Zero-copy loan API
auto loan = queue.try_loan();
if (loan) {
    loan->sequence = 1;
    loan->price = 50000.0;
    loan.commit();  // Publish
}
```

**Features**:
- Sub-microsecond latency
- Zero-copy loan/borrow API (iceoryx2-style)
- Batch operations for throughput
- Backpressure handling
- Flow control with credits
- Process crash recovery
- Service registry for discovery
- Heartbeat monitoring

## Performance

Benchmarked on Apple M4 Pro:

| Transport | Avg Latency | Throughput |
|-----------|-------------|------------|
| Raw UDP | 23us | 225K msg/s |
| SHM SPSC | <1us | >10M msg/s |
| SHM Batched | - | 50M+ msg/s |

## When to Use

**UDP**:
- Network communication (localhost or LAN)
- Can tolerate occasional packet loss
- Need lowest latency without reliability

**SHM**:
- Same-machine IPC
- Need zero-copy performance
- Multiple producer/consumer patterns

### KCP Transport (`asyncio/net/lowlatency/kcp.h`)

Reliable UDP transport using KCP (KCP-like ARQ protocol). Use cases:
- Game servers requiring reliable message delivery
- Applications needing UDP performance with TCP reliability
- Cross-network reliable streaming

**Performance**: ~18ms average RTT (vs ~1-2us raw UDP)

```cpp
#include <asyncio/net/lowlatency/kcp.h>

using namespace asyncio::net::lowlatency::kcp;

// Server
Config config;
config.mode = Mode::TURBO;  // Low latency mode

KcpTransport server(config);
server.bind(12345);

server.on_receive([](const void* data, size_t len, const Endpoint& sender) {
    // Handle received data - guaranteed delivery
});

server.start();

// Client
KcpTransport client(config);
client.bind(0);

Endpoint server_ep{"127.0.0.1", 12345};
client.connect(server_ep);
client.start();

// Send reliable data
client.send(server_ep, data, len);  // Automatic retransmission

client.stop();
server.stop();
```

**Features**:
- Reliable UDP with automatic retransmission (ARQ)
- 30-40% lower average RTT vs TCP
- Multiple modes: Normal, Fast, Turbo, Fastest
- Multicast group support
- Async/await API with asyncio coroutines
- Auto-session management per endpoint

**Modes**:
- `NORMAL`: Conservative, TCP-like (interval=40ms)
- `FAST`: Faster retransmission (interval=30ms, resend=2)
- `TURBO`: Low latency (interval=10ms, nodelay=1)
- `FASTEST`: Ultra-low latency (interval=20ms, rx_minrto=10)
- `RAW_UDP`: Bypass KCP for raw UDP (no reliability)

## Performance

Benchmarked on Apple M4 Pro:

| Transport | Avg Latency | Throughput |
|-----------|-------------|------------|
| Raw UDP | 23us | 225K msg/s |
| KCP (reliable) | 18ms | 72K msg/s |
| SHM SPSC | <1us | >10M msg/s |
| SHM Batched | - | 50M+ msg/s |

## When to Use

**UDP**:
- Network communication (localhost or LAN)
- Can tolerate occasional packet loss
- Need lowest latency without reliability

**KCP**:
- Need reliable delivery over UDP
- Lower latency than TCP is important
- Game servers, streaming, remote procedure calls

**SHM**:
- Same-machine IPC
- Need zero-copy performance
- Multiple producer/consumer patterns
