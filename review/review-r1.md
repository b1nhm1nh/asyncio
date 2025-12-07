# asyncio Framework Review R1

## Comparative Analysis with Boost.Asio and Drogon

**Date:** December 2025
**Version Reviewed:** 1.0.6
**Reviewer:** Technical Architecture Review

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Framework Comparison Matrix](#framework-comparison-matrix)
3. [Architecture Deep Dive](#architecture-deep-dive)
4. [Design Pattern Analysis](#design-pattern-analysis)
5. [API Ergonomics](#api-ergonomics)
6. [Performance Considerations](#performance-considerations)
7. [Feature Gap Analysis](#feature-gap-analysis)
8. [Strengths](#strengths)
9. [Areas for Improvement](#areas-for-improvement)
10. [Recommendations](#recommendations)
11. [Conclusion](#conclusion)

---

## Executive Summary

The asyncio framework represents a modern approach to asynchronous C++ networking, leveraging C++23 coroutines to deliver a clean, callback-free programming model. This review compares asyncio against two established frameworks:

- **Boost.Asio**: The industry-standard async I/O library for C++
- **Drogon**: A high-performance C++ HTTP application framework

### Key Findings

| Metric | asyncio | Boost.Asio | Drogon |
|--------|---------|------------|--------|
| API Simplicity | Excellent | Moderate | Good |
| Coroutine Support | Native C++23 | C++20 add-on | Callback-based |
| Task Composition | Best-in-class | Basic | Limited |
| Cancellation Model | Intuitive | Complex | Minimal |
| HTTP Server | Not available | Via Beast | Built-in |
| Ecosystem Maturity | Emerging | Mature | Growing |

---

## Framework Comparison Matrix

### 1. Language Standard & Compatibility

| Aspect | asyncio | Boost.Asio | Drogon |
|--------|---------|------------|--------|
| Minimum Standard | C++23 | C++11 | C++17 |
| Recommended Standard | C++23 | C++20 | C++20 |
| GCC Support | 14+ | 4.8+ | 8+ |
| Clang Support | 18+ | 3.4+ | 6+ |
| MSVC Support | 19.38+ | 14.0+ | 16.0+ |

**Analysis:** asyncio's C++23 requirement is both its strength and limitation. While it enables modern features like `std::expected` and improved ranges, it restricts adoption to projects using cutting-edge compilers. Boost.Asio's wide compatibility makes it suitable for legacy codebases.

### 2. Async Programming Model

#### asyncio: Coroutine-First Design

```cpp
asyncio::task::Task<void, std::error_code> fetchData() {
    auto stream = co_await TCPStream::connect("api.example.com", 443);
    CO_EXPECT(stream);

    co_await stream->writeAll(request);
    auto response = co_await stream->readAll();
    CO_EXPECT(response);

    co_return {};
}
```

**Characteristics:**
- Pure coroutine syntax throughout
- No callback registration
- Linear, synchronous-looking code flow
- Dual error handling (exceptions or `std::expected`)

#### Boost.Asio: Multi-Paradigm

```cpp
// Callback style
socket.async_read_some(buffer, [](error_code ec, size_t n) {
    // Handle completion
});

// C++20 coroutine style
asio::awaitable<void> fetchData() {
    auto [ec, n] = co_await socket.async_read_some(buffer, as_tuple(use_awaitable));
}

// Future style
std::future<size_t> f = socket.async_read_some(buffer, use_future);
```

**Characteristics:**
- Multiple completion token styles
- Backwards compatible with callback code
- Executors for thread management
- More complex but more flexible

#### Drogon: Callback-Centric

```cpp
client->sendRequest(req, [](ReqResult result, const HttpResponsePtr& resp) {
    if (result == ReqResult::Ok) {
        LOG_INFO << resp->body();
    }
});
```

**Characteristics:**
- Traditional callback model
- HTTP-focused abstractions
- Event loop per thread

### 3. Task Composition

| Operation | asyncio | Boost.Asio | Drogon |
|-----------|---------|------------|--------|
| Wait all succeed | `all(t1, t2, ...)` | `parallel_group` | N/A |
| Wait all complete | `allSettled(...)` | Manual | N/A |
| First success wins | `any(...)` | Manual | N/A |
| First complete wins | `race(...)` | `parallel_group` | N/A |
| Dynamic task group | `TaskGroup` | Manual | N/A |

**asyncio's task composition is a standout feature:**

```cpp
// Wait for all, cancel rest on first failure
auto [result1, result2] = co_await all(
    fetchUser(userId),
    fetchProfile(userId)
);

// Race: first completion wins
co_await race(
    processRequest(req),
    timeout(30s)
);

// Dynamic task management
TaskGroup group;
while (auto conn = co_await listener.accept()) {
    auto task = handleConnection(*std::move(conn));
    group.add(task);
}
co_await group;  // Wait for all handlers
```

**Boost.Asio requires more boilerplate:**

```cpp
auto [order, ex0, n0, ex1, n1] = co_await make_parallel_group(
    socket1.async_read_some(buffer1, deferred),
    socket2.async_read_some(buffer2, deferred)
).async_wait(
    wait_for_one(),
    use_awaitable
);
```

### 4. Cancellation Model

#### asyncio: Direct and Intuitive

```cpp
auto task = asyncio::sleep(1h);
task.cancel();  // Immediate, propagates through task tree
```

**Propagation through task tree:**
```
task.cancel()
├── test1.cancel()
│   ├── test3.cancel()
│   └── test4.cancel()
└── test2.cancel()
    ├── test5.cancel()
    └── test6.cancel()
```

**Key features:**
- `task.cancel()` inspired by Python's asyncio
- Cancellation propagates through linked task chains
- Each suspension point can register cancellation handler
- `co_await task::cancelled` to check cancellation status

#### Boost.Asio: Signal-Based

```cpp
asio::cancellation_signal sig;
auto slot = sig.slot();

co_spawn(ctx, [slot]() -> awaitable<void> {
    co_await async_op(..., bind_cancellation_slot(slot, use_awaitable));
}, detached);

sig.emit(cancellation_type::total);  // Request cancellation
```

**Characteristics:**
- More explicit wiring required
- Three cancellation types (total, partial, terminal)
- Requires threading cancellation_slot through operations

#### Drogon: Minimal Support

No structured cancellation mechanism. Operations complete or timeout.

### 5. Error Handling

#### asyncio: Dual-Mode with std::expected

```cpp
// Mode 1: Error codes (explicit handling)
Task<std::string, std::error_code> readFile(std::string path) {
    auto file = co_await fs::open(path);
    CO_EXPECT(file);  // Returns error if failed

    auto content = co_await file->readAll();
    CO_EXPECT(content);

    co_return *content;
}

// Mode 2: Exceptions (implicit handling)
Task<std::string> readFile(std::string path) {
    auto file = co_await fs::open(path);  // Throws on error
    co_return co_await file->readAll();   // Throws on error
}
```

**Custom error codes with categories:**
```cpp
DEFINE_ERROR_CODE_EX(
    Error,
    "asyncio::task",
    CANCELLED, "task has been cancelled", std::errc::operation_canceled,
    TIMEOUT, "operation timed out", std::errc::timed_out
)
```

#### Boost.Asio: error_code or Exceptions

```cpp
// Error code style
auto [ec, n] = co_await socket.async_read(buffer, as_tuple(use_awaitable));
if (ec) { /* handle */ }

// Exception style (throws on error)
size_t n = co_await socket.async_read(buffer, use_awaitable);
```

#### Drogon: HTTP-Centric Errors

Error handling focused on HTTP status codes and request results.

---

## Architecture Deep Dive

### asyncio Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application Layer                         │
├─────────────────────────────────────────────────────────────────┤
│  HTTP Client │ WebSocket │ TLS/SSL │ Process │ Filesystem │ ... │
├─────────────────────────────────────────────────────────────────┤
│              Networking Layer (TCP/UDP/DNS)                      │
├─────────────────────────────────────────────────────────────────┤
│    Channel  │  Sync Primitives  │  Buffer I/O  │  Streams       │
├─────────────────────────────────────────────────────────────────┤
│                     Task & Coroutine Layer                       │
│   Task<T,E> │ TaskGroup │ all/any/race │ Cancellation │ Frame   │
├─────────────────────────────────────────────────────────────────┤
│                   Promise/Future Layer                           │
├─────────────────────────────────────────────────────────────────┤
│                  Event Loop (thread-local)                       │
├─────────────────────────────────────────────────────────────────┤
│                        libuv                                     │
└─────────────────────────────────────────────────────────────────┘
```

**Key architectural decisions:**

1. **libuv Foundation**: Provides cross-platform event loop, handles platform-specific I/O multiplexing (epoll, kqueue, IOCP)

2. **Thread-local Event Loop**: Accessed via `getEventLoop()`, avoids passing context everywhere

3. **Frame-based Task Tracking**: Linked list of frames enables cancellation propagation and debugging (stack traces)

4. **Promise Bridge**: Connects libuv callbacks to coroutine resumption

### Boost.Asio Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                       Application Code                           │
├─────────────────────────────────────────────────────────────────┤
│            Composed Operations / Coroutines                      │
├─────────────────────────────────────────────────────────────────┤
│    Completion Tokens: callback, future, awaitable, deferred     │
├─────────────────────────────────────────────────────────────────┤
│                      Async Operations                            │
├─────────────────────────────────────────────────────────────────┤
│          Executors & Execution Context                           │
├─────────────────────────────────────────────────────────────────┤
│              Proactor (completion-based) Model                   │
├─────────────────────────────────────────────────────────────────┤
│         Platform I/O: IOCP, epoll, kqueue, select               │
└─────────────────────────────────────────────────────────────────┘
```

**Key differences from asyncio:**
- Direct platform I/O (no libuv abstraction)
- Executor concept for thread pool management
- Strand for serialization without explicit locking
- More layers of abstraction

### Drogon Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│              Application Controllers & Filters                   │
├─────────────────────────────────────────────────────────────────┤
│     Routing │ Middleware │ ORM │ Templates │ WebSocket          │
├─────────────────────────────────────────────────────────────────┤
│                    HTTP Protocol Handler                         │
├─────────────────────────────────────────────────────────────────┤
│           Event Loop per Thread (Thread Pool)                    │
├─────────────────────────────────────────────────────────────────┤
│              Platform I/O: epoll / kqueue                        │
└─────────────────────────────────────────────────────────────────┘
```

**Key characteristics:**
- HTTP-centric design
- Thread pool with event loop per thread
- Integrated ORM and view rendering
- Less general-purpose than asyncio or Boost.Asio

---

## Design Pattern Analysis

### 1. Interface-Based Design (asyncio)

```cpp
class IReader : public virtual zero::Interface {
public:
    virtual Task<size_t, std::error_code> read(std::span<std::byte> data) = 0;
    virtual Task<void, std::error_code> readExactly(std::span<std::byte> data);
    virtual Task<std::vector<std::byte>, std::error_code> readAll();
};

class IWriter : public virtual zero::Interface {
public:
    virtual Task<size_t, std::error_code> write(std::span<const std::byte> data) = 0;
    virtual Task<void, std::error_code> writeAll(std::span<const std::byte> data);
};

class ICloseable : public virtual zero::Interface {
public:
    virtual Task<void, std::error_code> close() = 0;
};
```

**Benefits:**
- Composable interfaces (TCPStream implements IReader + IWriter + ICloseable)
- Easy mocking for testing
- Clear separation of concerns

**Comparison:** Boost.Asio uses concept-based duck typing rather than virtual interfaces, which offers better performance but less explicit contracts.

### 2. RAII Resource Management

asyncio uses move semantics and unique ownership:

```cpp
auto stream = co_await TCPStream::connect(host, port);
CO_EXPECT(stream);
// stream is moved, automatically cleaned up when scope exits

// Explicit close is optional but available
co_await stream->close();
```

### 3. Channel Communication (Go-inspired)

```cpp
auto [sender, receiver] = Channel<Message>::make(100);  // Buffer size 100

// Producer task
co_await sender.send(message);

// Consumer task
while (true) {
    auto msg = co_await receiver.receive();
    if (!msg) break;
    process(*msg);
}
```

**Neither Boost.Asio nor Drogon provides built-in channels.**

### 4. Synchronization Primitives

```cpp
// Async mutex
sync::Mutex mutex;
auto guard = co_await mutex.lock();
// Critical section
// guard destructor releases lock

// Async condition variable
sync::Condition cond;
co_await cond.wait(lock, [] { return ready; });
cond.notifyOne();
```

**asyncio provides async-aware sync primitives not available in standard Boost.Asio.**

---

## API Ergonomics

### Code Comparison: TCP Echo Server

#### asyncio

```cpp
Task<void, std::error_code> handle(TCPStream stream) {
    while (true) {
        std::array<std::byte, 1024> buffer;
        auto n = co_await stream.read(buffer);
        CO_EXPECT(n);
        if (*n == 0) break;
        CO_EXPECT(co_await stream.writeAll({buffer.data(), *n}));
    }
    co_return {};
}

Task<void, std::error_code> serve(TCPListener listener) {
    TaskGroup group;
    while (auto stream = co_await listener.accept()) {
        if (!stream) break;
        group.add(handle(*std::move(stream)));
    }
    co_await group;
    co_return {};
}
```

**Lines of code:** ~20
**Cognitive complexity:** Low

#### Boost.Asio (C++20 coroutines)

```cpp
awaitable<void> handle(tcp::socket socket) {
    try {
        std::array<char, 1024> buffer;
        for (;;) {
            size_t n = co_await socket.async_read_some(
                asio::buffer(buffer), use_awaitable);
            co_await async_write(socket,
                asio::buffer(buffer, n), use_awaitable);
        }
    } catch (std::exception&) {}
}

awaitable<void> serve(tcp::acceptor& acceptor) {
    for (;;) {
        auto socket = co_await acceptor.async_accept(use_awaitable);
        co_spawn(acceptor.get_executor(), handle(std::move(socket)), detached);
    }
}
```

**Lines of code:** ~22
**Cognitive complexity:** Medium (requires understanding executors, completion tokens)

#### Drogon

```cpp
void handle(const TcpConnectionPtr& conn, MsgBuffer* buffer) {
    conn->send(buffer->peek(), buffer->readableBytes());
    buffer->retrieveAll();
}

// Setup in main
app().registerHandler("/echo", &handle);
app().run();
```

**Lines of code:** ~8 (but HTTP-only, not raw TCP)
**Cognitive complexity:** Low for HTTP, limited for raw protocols

### Verdict on API Ergonomics

| Criteria | asyncio | Boost.Asio | Drogon |
|----------|---------|------------|--------|
| Learning curve | Gentle | Steep | Moderate |
| Boilerplate | Minimal | Moderate | Low |
| Error handling clarity | Excellent | Good | Good |
| Debugging experience | Good (stack traces) | Challenging | Moderate |

---

## Performance Considerations

### Event Loop Implementation

| Framework | Event Loop | Notes |
|-----------|------------|-------|
| asyncio | libuv | Battle-tested, cross-platform, possible abstraction overhead |
| Boost.Asio | Native | Direct epoll/kqueue/IOCP, maximum performance potential |
| Drogon | Native | Direct epoll/kqueue, Linux-optimized |

### Potential Performance Tradeoffs

1. **libuv Abstraction Layer**: asyncio builds on libuv, adding a layer between the application and native I/O. While libuv is highly optimized (used by Node.js), direct native I/O in Boost.Asio may have marginally lower latency in extreme cases.

2. **Virtual Interface Overhead**: asyncio's use of virtual interfaces (IReader, IWriter) adds vtable lookups. Boost.Asio's concept-based approach enables more inlining opportunities.

3. **Task Frame Allocation**: Each task in asyncio allocates a Frame for cancellation/debugging. This is minimal overhead but exists.

4. **Promise/Future Bridge**: The conversion between libuv callbacks and coroutine resumption requires promise allocation and resolution.

### When Performance Matters Most

- **High-frequency trading**: Boost.Asio (raw) or custom solution
- **Web applications**: All three are adequate; Drogon optimized for HTTP
- **General networking**: asyncio's overhead is negligible for most cases
- **Microservices**: All three suitable; choose based on ecosystem needs

---

## Feature Gap Analysis

### Features Present in Competitors but Missing in asyncio

| Feature | Boost.Asio | Drogon | Priority for asyncio |
|---------|------------|--------|----------------------|
| HTTP Server | Via Beast | Built-in | **High** |
| io_uring support | Available | Partial | Medium |
| Executor customization | Yes | N/A | Medium |
| Connection pooling | Via Beast | Yes | Low |
| ORM | No | Yes | Low |
| Template rendering | No | Yes | Low |

### Features Unique to asyncio

| Feature | Value Proposition |
|---------|-------------------|
| `all/any/race/allSettled` | Best-in-class task composition |
| `TaskGroup` | Dynamic task collection management |
| Direct `task.cancel()` | Intuitive cancellation model |
| `Channel<T>` | Go-style inter-task communication |
| Async sync primitives | Mutex, Event, Condition for coroutines |
| Dual error handling | Choice of exceptions or error codes |
| Task call tree | Debugging/tracing support |

---

## Strengths

### 1. Modern C++ Design

asyncio embraces C++23 fully:
- `std::expected` for error handling
- Ranges library integration
- `deducing this` for coroutine safety
- Clean, modern syntax throughout

### 2. Intuitive API

The API closely mirrors Python's asyncio, making it accessible to developers familiar with async/await patterns:

```cpp
co_await asyncio::sleep(1s);
co_await all(task1(), task2());
task.cancel();
```

### 3. Superior Task Composition

No other C++ framework offers such ergonomic task composition:

```cpp
// Timeout any operation
co_await asyncio::timeout(fetch(), 30s);

// Wait for first success
auto result = co_await any(
    fetchFromPrimary(),
    fetchFromBackup()
);

// Graceful shutdown pattern
co_await race(
    serve(listener),
    signal->on(SIGINT)
);
```

### 4. Elegant Cancellation

The `task.cancel()` model is simpler than context propagation:

```cpp
auto longTask = computeForever();

// Later, when needed:
if (shutdownRequested) {
    longTask.cancel();
    co_await longTask;  // Completes with CANCELLED error
}
```

### 5. Comprehensive Documentation

The `/doc` directory provides:
- Philosophical introduction explaining design decisions
- Detailed overview of core concepts
- Error handling guide with practical examples
- API reference for each module
- Bilingual support (English and Chinese)

### 6. Sync Primitive Support

Async-aware synchronization not found in competitors:

```cpp
sync::Mutex mutex;
sync::Event event;
sync::Condition condition;

co_await mutex.lock();
co_await event.wait();
co_await condition.wait(lock, predicate);
```

---

## Areas for Improvement

### 1. HTTP Server (Critical Gap)

The roadmap acknowledges this. Without an HTTP server, asyncio cannot compete with Drogon for web applications:

**Recommendation:** Implement a coroutine-native HTTP/1.1 and HTTP/2 server. Consider:
- Parser: llhttp (used by Node.js) or custom
- HTTP/2: nghttp2 integration
- Design: Request handler as `Task<Response, Error>`

### 2. io_uring Support

Linux's io_uring provides significant performance improvements for I/O-bound applications:

**Recommendation:**
- Investigate libuv's io_uring support status
- Consider optional native io_uring backend for Linux

### 3. Executor/Scheduler Customization

Boost.Asio's executor concept allows flexible thread pool management:

```cpp
// Boost.Asio style
auto executor = make_strand(pool.get_executor());
co_spawn(executor, my_task, detached);
```

**Recommendation:** Consider adding pluggable scheduler/executor for:
- Custom thread pool sizes
- Priority scheduling
- Work stealing

### 4. Connection Pooling

For HTTP client heavy workloads:

**Recommendation:** The HTTP client could benefit from:
- Configurable connection pool per host
- Keep-alive management
- Connection health checking

### 5. Observability

Production systems need instrumentation:

**Recommendation:**
- OpenTelemetry integration for tracing
- Metrics hooks for monitoring
- Structured logging support

### 6. Compiler Support Expansion

C++23 requirement limits adoption:

**Recommendation:**
- Consider C++20 fallback mode with polyfills for `std::expected`
- Document which features degrade in C++20 mode

---

## Recommendations

### Short-term (Next Release)

1. **Implement HTTP Server**
   - Start with HTTP/1.1
   - Coroutine-native request handlers
   - Example:
     ```cpp
     server.route("/api/users", [](Request req) -> Task<Response> {
         auto users = co_await db.query("SELECT * FROM users");
         co_return Response::json(users);
     });
     ```

2. **Add Benchmarks**
   - Compare against Boost.Asio and Drogon
   - Publish results in documentation
   - Common scenarios: echo server, HTTP throughput

3. **CI/CD for Multiple Compilers**
   - Test matrix: GCC 14, Clang 18, MSVC latest
   - Document any compiler-specific limitations

### Medium-term

4. **io_uring Investigation**
   - Profile current libuv performance
   - Prototype io_uring backend if significant gains possible

5. **Executor Concept**
   - Allow custom schedulers
   - Thread pool configuration
   - Per-task executor binding

6. **Observability Hooks**
   - Task lifecycle events
   - I/O operation metrics
   - Integration points for tracing

### Long-term

7. **HTTP/2 and HTTP/3 Support**
   - Modern protocol support for the HTTP server
   - QUIC investigation

8. **gRPC Integration**
   - Native coroutine gRPC client/server
   - Competition with grpc++ async API

9. **Ecosystem Growth**
   - Database drivers (PostgreSQL, MySQL, Redis)
   - Message queues (Kafka, RabbitMQ)
   - Cloud SDK integrations

---

## Conclusion

### Summary Assessment

**asyncio** is an impressive modern C++ async framework that prioritizes developer experience without sacrificing capability. Its coroutine-first design, intuitive cancellation model, and superior task composition APIs set it apart from established alternatives.

| Category | Rating | Notes |
|----------|--------|-------|
| API Design | ★★★★★ | Best-in-class ergonomics |
| Coroutine Support | ★★★★★ | Native C++23 design |
| Task Management | ★★★★★ | all/any/race/TaskGroup |
| Error Handling | ★★★★★ | Dual-mode with std::expected |
| Cancellation | ★★★★★ | Simple and effective |
| Documentation | ★★★★☆ | Comprehensive, bilingual |
| Feature Completeness | ★★★☆☆ | Missing HTTP server |
| Ecosystem Maturity | ★★★☆☆ | Emerging project |
| Performance | ★★★★☆ | libuv overhead vs native |
| Adoption Potential | ★★★☆☆ | C++23 requirement limits reach |

### When to Choose asyncio

**Choose asyncio when:**
- Starting a new C++23 project
- Developer experience is a priority
- Complex task coordination is required
- You need channels and async sync primitives
- Clean error handling matters

**Choose Boost.Asio when:**
- Maximum performance is critical
- Legacy codebase compatibility needed
- Ecosystem integration required (Beast, Process, etc.)
- Team has existing Asio expertise

**Choose Drogon when:**
- Building HTTP web applications
- Need integrated ORM and templates
- HTTP server is the primary requirement
- Benchmark performance for web workloads

### Final Verdict

asyncio successfully delivers on its promise of making "C++ network programming simpler." The framework occupies a unique position in the C++ async ecosystem—more modern than Boost.Asio, more general-purpose than Drogon. With the addition of an HTTP server and continued ecosystem development, asyncio has strong potential to become a leading choice for modern C++ network applications.

The framework demonstrates that embracing new C++ standards can yield dramatically better developer experiences. For teams willing to adopt C++23, asyncio offers compelling advantages that outweigh its current limitations.

---

## References

- [Boost.Asio Documentation](https://www.boost.org/doc/libs/1_86_0/doc/html/boost_asio.html)
- [Boost.Asio Proactor Pattern](https://www.boost.org/doc/libs/1_86_0/doc/html/boost_asio/overview/core/async.html)
- [Drogon Framework](https://github.com/drogonframework/drogon)
- [libuv Documentation](https://libuv.org/)
- [C++23 std::expected](https://en.cppreference.com/w/cpp/utility/expected)
- [ASIO Design Patterns](https://softwarepatternslexicon.com/patterns-cpp/11/6/)

---

*Review prepared for asyncio version 1.0.6*
