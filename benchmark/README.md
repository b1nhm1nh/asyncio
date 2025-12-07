# asyncio Benchmarks

Comparative benchmarks between asyncio, standalone Asio, and Drogon.

## Building

```bash
# Configure with benchmarks enabled
cmake --preset debug -DBUILD_BENCHMARKS=ON

# Build
cmake --build --preset debug
```

## TCP Echo Benchmarks

Compares TCP echo performance between asyncio and standalone Asio.

```bash
# Run TCP benchmarks (servers start automatically)
./build/debug/benchmark/bench_tcp_echo
```

### What's measured
- Echo latency for various message sizes (64B, 1KB, 64KB)
- Throughput (bytes/second) for sustained transfers
- Round-trip times for request/response patterns

## WebSocket Benchmarks

Compares WebSocket client performance between asyncio and Drogon.

**Note:** Requires an external WebSocket echo server running on port 18081.

```bash
# Start an echo server (using websocat or similar)
websocat -s 127.0.0.1:18081

# Or use any WebSocket echo server

# Run WebSocket benchmarks
./build/debug/benchmark/bench_websocket
```

### What's measured
- Text message echo latency
- Binary message echo latency
- Throughput for various payload sizes

## Benchmark Configuration

Edit `common.h` to adjust:
- `TCP_PORT`: Port for TCP echo server (default: 18080)
- `WS_PORT`: Port for WebSocket echo server (default: 18081)
- `LOCALHOST`: Bind address (default: 127.0.0.1)

## Sample Output

```
---------------------------------------------------------------
Benchmark                     Time             CPU   Iterations
---------------------------------------------------------------
BM_AsyncioTcpEcho_64B/100   xxx ms          xxx ms          xxx
BM_AsioTcpEcho_64B/100      xxx ms          xxx ms          xxx
...
```

## Notes

- TCP benchmarks include both client and server in the same process
- WebSocket benchmarks require external echo server for fair comparison
- Results vary significantly based on system load and hardware
- Run multiple times for consistent results
