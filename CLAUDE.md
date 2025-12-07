# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

asyncio is a C++23 coroutine-based network framework built on libuv. It provides callback-free async programming using stackless coroutines with `Task<T, E>` as the core abstraction.

**Requirements:** GCC 14+, LLVM 18+, or MSVC 19.38+ with C++23 support. Requires `VCPKG_ROOT` environment variable.

## Build Commands

```bash
# Full build + test workflow (recommended)
cmake --workflow --preset debug

# Individual steps
cmake --preset debug                    # Configure
cmake --build --preset debug            # Build
ctest --preset debug                    # Run all tests

# With AddressSanitizer
cmake --workflow --preset debug-asan

# Release build
cmake --workflow --preset release
```

## Running Tests

Tests use Catch2. After building:

```bash
# Run all tests
ctest --preset debug

# Run specific test by name pattern
ctest --preset debug -R "channel"
ctest --preset debug -R "net"

# Run single test executable directly with Catch2 filters
./build/debug/test/asyncio_test "[channel]"
./build/debug/test/asyncio_test "test name"
```

## Architecture

### Core Layers (bottom-up)

1. **libuv** - Cross-platform event loop (epoll/kqueue/IOCP)
2. **Event Loop** (`event_loop.h`) - Thread-local via `getEventLoop()`, wraps libuv
3. **Promise/Future** (`promise.h`) - Bridges libuv callbacks to coroutine resumption
4. **Task** (`task.h`) - Core coroutine type `Task<T, E>` with frame-based cancellation

### Task System

```cpp
// Two error modes:
Task<Result, std::error_code>  // Error codes (use CO_EXPECT)
Task<Result>                   // Exceptions (throws on error)

// Task composition
all(t1, t2, ...)       // All must succeed, cancels rest on failure
allSettled(...)        // Wait for all, never fails
any(...)               // First success wins
race(...)              // First completion wins
TaskGroup              // Dynamic task collection
```

### Module Organization

- **Core:** `task.h`, `event_loop.h`, `promise.h`, `channel.h`, `thread.h`, `time.h`
- **I/O:** `io.h` (IReader/IWriter interfaces), `buffer.h`, `stream.h`, `fs.h`
- **Net:** `net/stream.h` (TCP), `net/dgram.h` (UDP), `net/dns.h`, `net/tls.h`
- **HTTP:** `http/request.h` (client), `http/websocket.h`
- **Sync:** `sync/mutex.h`, `sync/event.h`, `sync/condition.h`

### Entry Point Pattern

Link against `asyncio-main` and implement:

```cpp
asyncio::task::Task<void, std::error_code> asyncMain(int argc, char *argv[]);
```

Or use `asyncio::run()` directly:

```cpp
int main() {
    auto result = asyncio::run([]() -> Task<void> { ... });
}
```

### Error Handling

- `CO_EXPECT(expr)` - Early return on error in coroutines
- `EXPECT(expr)` - Early return on error in regular functions
- Custom errors via `DEFINE_ERROR_CODE_EX` macro with category and condition mapping

### Cancellation

Direct model: `task.cancel()` propagates through linked task tree. Register cancellation handlers via `CancellableFuture`.

## Dependencies

Managed via vcpkg: libuv, OpenSSL, CURL, nlohmann_json, zlib, zero (internal utility library)
