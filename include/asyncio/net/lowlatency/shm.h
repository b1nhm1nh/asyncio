// Asyncio Shared Memory IPC - Full-Featured Async/Await IPC Library
// Combines performance of beam_ipc/iceoryx2 with asyncio's coroutine model
//
// Features:
// - Zero-copy shared memory communication
// - Multiple patterns: SPSC, SPMC (Pub/Sub), MPSC, Request/Response
// - Async/await API using asyncio::task::Task
// - Event notifications (no busy polling)
// - Memory pool with loan/borrow API
// - Process crash recovery
// - Cross-platform (POSIX + Windows)
// - C++23 features: std::expected, concepts, constexpr, deducing this
//
// Error Handling:
// - Uses std::error_code with custom error category (asyncio pattern)
// - Compatible with std::errc error conditions
// - Uses zero/error.h DEFINE_ERROR_CODE_EX macro
//
// Inspired by: beam_ipc.h, iceoryx2, Aeron

#pragma once

#include <asyncio/task.h>
#include <asyncio/event_loop.h>
#include <asyncio/time.h>
#include <zero/error.h>
#include <zero/expect.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <span>
#include <bit>
#include <optional>
#include <expected>
#include <chrono>
#include <thread>
#include <array>
#include <concepts>
#include <memory>
#include <functional>
#include <system_error>

// Platform-specific includes
#ifdef _WIN32
    #include <windows.h>
    #include <synchapi.h>
#else
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
    #ifdef __linux__
        #include <linux/futex.h>
        #include <sys/syscall.h>
    #endif
#endif

namespace asyncio::net::lowlatency::shm {

// =============================================================================
// Configuration Constants
// =============================================================================

inline constexpr size_t CACHE_LINE_SIZE = 64;
inline constexpr size_t MAX_SUBSCRIBERS = 16;
inline constexpr uint64_t MAGIC_SPSC = 0xA5105053C001ULL;
inline constexpr uint64_t MAGIC_SPMC = 0xA510505C0002ULL;
inline constexpr uint64_t MAGIC_MPSC = 0xA5104D5C0003ULL;
inline constexpr uint64_t MAGIC_RPC  = 0xA510525C0004ULL;
inline constexpr uint64_t MAGIC_POOL = 0xA510504C0005ULL;

// =============================================================================
// C++23 Concepts
// =============================================================================

template<typename T>
concept TrivialMessage = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

template<typename T>
concept ShmCompatible = TrivialMessage<T> && (alignof(T) <= CACHE_LINE_SIZE);

// =============================================================================
// Error Handling (asyncio-style with std::error_code)
// =============================================================================

// Define error codes using zero/error.h macros (same pattern as asyncio)
DEFINE_ERROR_CODE_EX(
    Error,
    "asyncio_shm",
    QUEUE_FULL, "queue is full", std::errc::no_buffer_space,
    QUEUE_EMPTY, "queue is empty", std::errc::resource_unavailable_try_again,
    DISCONNECTED, "endpoint disconnected", std::errc::broken_pipe,
    TIMEOUT, "operation timed out", std::errc::timed_out,
    NO_MEMORY, "out of memory", std::errc::not_enough_memory,
    INVALID_STATE, "invalid state", std::errc::invalid_argument,
    TOO_MANY_SUBSCRIBERS, "too many subscribers", std::errc::too_many_files_open
)

// Result type using std::error_code (compatible with asyncio)
template<typename T>
using Result = std::expected<T, std::error_code>;

} // namespace asyncio::net::lowlatency::shm

// Declare Error as an error_code enum (required outside namespace)
DECLARE_ERROR_CODE(asyncio::net::lowlatency::shm::Error)

// Define error category instance inline (for header-only library)
// This is equivalent to DEFINE_ERROR_CATEGORY_INSTANCE but inline
inline const std::error_category& asyncio::net::lowlatency::shm::ErrorCategory::instance() {
    return errorCategoryInstance<asyncio::net::lowlatency::shm::ErrorCategory>();
}

// Re-open namespace for rest of implementation
namespace asyncio::net::lowlatency::shm {

// =============================================================================
// Platform Abstraction Layer
// =============================================================================

namespace platform {

#ifdef _WIN32

inline void* shm_create(const std::string& name, size_t size) {
    std::string mapped_name = "Local\\" + name;
    HANDLE hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xFFFFFFFF),
        mapped_name.c_str());
    if (!hMapFile) return nullptr;
    void* ptr = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, size);
    return ptr;
}

inline void* shm_open_existing(const std::string& name, size_t& size) {
    std::string mapped_name = "Local\\" + name;
    HANDLE hMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapped_name.c_str());
    if (!hMapFile) return nullptr;
    void* ptr = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    return ptr;
}

inline void shm_close(void* ptr, size_t) {
    if (ptr) UnmapViewOfFile(ptr);
}

inline void shm_unlink(const std::string&) {}

// Event notification using WaitOnAddress
inline void event_wait(std::atomic<uint32_t>* flag, uint32_t expected) {
    WaitOnAddress(flag, &expected, sizeof(uint32_t), INFINITE);
}

inline void event_wake_one(std::atomic<uint32_t>* flag) {
    WakeByAddressSingle(flag);
}

inline void event_wake_all(std::atomic<uint32_t>* flag) {
    WakeByAddressAll(flag);
}

#else  // POSIX

inline void* shm_create(const std::string& name, size_t size) {
    std::string shm_name = "/" + name;
    ::shm_unlink(shm_name.c_str());

    int fd = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) return nullptr;

    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    return (ptr == MAP_FAILED) ? nullptr : ptr;
}

inline void* shm_open_existing(const std::string& name, size_t& out_size) {
    std::string shm_name = "/" + name;
    int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0666);
    if (fd < 0) return nullptr;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return nullptr;
    }
    out_size = static_cast<size_t>(st.st_size);

    void* ptr = mmap(nullptr, out_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    return (ptr == MAP_FAILED) ? nullptr : ptr;
}

inline void shm_close(void* ptr, size_t size) {
    if (ptr) munmap(ptr, size);
}

inline void shm_unlink(const std::string& name) {
    std::string shm_name = "/" + name;
    ::shm_unlink(shm_name.c_str());
}

#ifdef __linux__
// Linux futex-based event notification
inline void event_wait(std::atomic<uint32_t>* flag, uint32_t expected) {
    syscall(SYS_futex, flag, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0);
}

inline void event_wake_one(std::atomic<uint32_t>* flag) {
    syscall(SYS_futex, flag, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}

inline void event_wake_all(std::atomic<uint32_t>* flag) {
    syscall(SYS_futex, flag, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}
#else
// macOS - use spin wait (no futex)
inline void event_wait(std::atomic<uint32_t>* flag, uint32_t expected) {
    while (flag->load(std::memory_order_acquire) == expected) {
        std::this_thread::yield();
    }
}

inline void event_wake_one(std::atomic<uint32_t>*) {}
inline void event_wake_all(std::atomic<uint32_t>*) {}
#endif

#endif

}  // namespace platform

// =============================================================================
// AsyncEvent - Efficient Event Notification
// =============================================================================

class AsyncEvent {
public:
    struct alignas(CACHE_LINE_SIZE) SharedData {
        uint64_t magic{0};
        std::atomic<uint32_t> flag{0};
        std::atomic<uint32_t> waiters{0};
    };

    static AsyncEvent create(const std::string& name) {
        size_t size = sizeof(SharedData);
        void* ptr = platform::shm_create(name, size);
        if (!ptr) throw std::runtime_error("Failed to create event: " + name);

        auto* data = new (ptr) SharedData{};
        data->magic = 0xA510E7E7ULL;
        return AsyncEvent(name, ptr, size, true, data);
    }

    static AsyncEvent open(const std::string& name) {
        size_t size = 0;
        void* ptr = platform::shm_open_existing(name, size);
        if (!ptr) throw std::runtime_error("Failed to open event: " + name);

        auto* data = static_cast<SharedData*>(ptr);
        return AsyncEvent(name, ptr, size, false, data);
    }

    ~AsyncEvent() {
        if (ptr_) {
            platform::shm_close(ptr_, size_);
            if (owner_) platform::shm_unlink(name_);
        }
    }

    void set() {
        data_->flag.store(1, std::memory_order_release);
        platform::event_wake_all(&data_->flag);
    }

    void reset() {
        data_->flag.store(0, std::memory_order_release);
    }

    [[nodiscard]] bool is_set() const {
        return data_->flag.load(std::memory_order_acquire) != 0;
    }

    // Async wait - yields to event loop while waiting
    asyncio::task::Task<void> wait() {
        while (!is_set()) {
            co_await asyncio::sleep(std::chrono::milliseconds{1});
        }
    }

    // Async wait with timeout
    asyncio::task::Task<bool> wait_timeout(std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!is_set()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                co_return false;
            }
            co_await asyncio::sleep(std::chrono::milliseconds{1});
        }
        co_return true;
    }

    AsyncEvent(AsyncEvent&& o) noexcept
        : name_(std::move(o.name_)), ptr_(o.ptr_), size_(o.size_),
          owner_(o.owner_), data_(o.data_) { o.ptr_ = nullptr; }

    AsyncEvent(const AsyncEvent&) = delete;
    AsyncEvent& operator=(const AsyncEvent&) = delete;

private:
    AsyncEvent(std::string name, void* ptr, size_t size, bool owner, SharedData* data)
        : name_(std::move(name)), ptr_(ptr), size_(size), owner_(owner), data_(data) {}

    std::string name_;
    void* ptr_{nullptr};
    size_t size_{0};
    bool owner_{false};
    SharedData* data_{nullptr};
};

// =============================================================================
// AsyncSPSCQueue - Single Producer, Single Consumer (Enhanced)
// =============================================================================

template<ShmCompatible T>
class AsyncSPSCQueue {
public:
    struct alignas(CACHE_LINE_SIZE) Header {
        uint64_t magic{0};
        uint64_t capacity{0};
        uint64_t element_size{0};
    };

    struct alignas(CACHE_LINE_SIZE) ProducerData {
        std::atomic<uint64_t> write_pos{0};
        uint64_t cached_read_pos{0};
        std::atomic<uint32_t> notify_flag{0};
    };

    struct alignas(CACHE_LINE_SIZE) ConsumerData {
        std::atomic<uint64_t> read_pos{0};
        uint64_t cached_write_pos{0};
        std::atomic<uint32_t> notify_flag{0};
        std::atomic<uint32_t> borrow_in_progress{0};  // Track outstanding borrow
    };

    static AsyncSPSCQueue create(const std::string& name, size_t capacity) {
        capacity = std::bit_ceil(capacity);

        size_t total_size = sizeof(Header) + sizeof(ProducerData) +
                           sizeof(ConsumerData) + sizeof(T) * capacity;

        void* ptr = platform::shm_create(name, total_size);
        if (!ptr) throw std::runtime_error("Failed to create SPSC queue: " + name);

        std::memset(ptr, 0, total_size);

        char* p = static_cast<char*>(ptr);
        auto* header = new (p) Header{};
        auto* producer = new (p + sizeof(Header)) ProducerData{};
        auto* consumer = new (p + sizeof(Header) + sizeof(ProducerData)) ConsumerData{};
        auto* data = reinterpret_cast<T*>(p + sizeof(Header) + sizeof(ProducerData) + sizeof(ConsumerData));

        header->capacity = capacity;
        header->element_size = sizeof(T);
        std::atomic_thread_fence(std::memory_order_release);
        header->magic = MAGIC_SPSC;

        return AsyncSPSCQueue(name, ptr, total_size, true, header, producer, consumer, data, capacity);
    }

    static AsyncSPSCQueue open(const std::string& name) {
        size_t total_size = 0;
        void* ptr = platform::shm_open_existing(name, total_size);
        if (!ptr) throw std::runtime_error("Failed to open SPSC queue: " + name);

        auto* header = static_cast<Header*>(ptr);

        int retries = 1000;
        while (header->magic != MAGIC_SPSC && retries-- > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        if (header->magic != MAGIC_SPSC) {
            platform::shm_close(ptr, total_size);
            throw std::runtime_error("Invalid SPSC queue format");
        }

        char* p = static_cast<char*>(ptr);
        auto* producer = reinterpret_cast<ProducerData*>(p + sizeof(Header));
        auto* consumer = reinterpret_cast<ConsumerData*>(p + sizeof(Header) + sizeof(ProducerData));
        auto* data = reinterpret_cast<T*>(p + sizeof(Header) + sizeof(ProducerData) + sizeof(ConsumerData));

        return AsyncSPSCQueue(name, ptr, total_size, false, header, producer, consumer, data, header->capacity);
    }

    ~AsyncSPSCQueue() {
        if (ptr_) {
            platform::shm_close(ptr_, size_);
            if (owner_) platform::shm_unlink(name_);
        }
    }

    // Non-blocking operations
    [[nodiscard]] Result<void> try_send(const T& value) {
        uint64_t write_pos = producer_->write_pos.load(std::memory_order_relaxed);
        uint64_t read_pos = producer_->cached_read_pos;

        if (write_pos - read_pos >= capacity_) {
            read_pos = consumer_->read_pos.load(std::memory_order_acquire);
            producer_->cached_read_pos = read_pos;
            if (write_pos - read_pos >= capacity_) {
                return std::unexpected(Error::QUEUE_FULL);
            }
        }

        data_[write_pos & mask_] = value;
        producer_->write_pos.store(write_pos + 1, std::memory_order_release);
        producer_->notify_flag.store(1, std::memory_order_release);
        platform::event_wake_one(&producer_->notify_flag);
        return {};
    }

    [[nodiscard]] Result<T> try_receive() {
        uint64_t read_pos = consumer_->read_pos.load(std::memory_order_relaxed);
        uint64_t write_pos = consumer_->cached_write_pos;

        if (read_pos >= write_pos) {
            write_pos = producer_->write_pos.load(std::memory_order_acquire);
            consumer_->cached_write_pos = write_pos;
            if (read_pos >= write_pos) {
                return std::unexpected(Error::QUEUE_EMPTY);
            }
        }

        T value = data_[read_pos & mask_];
        consumer_->read_pos.store(read_pos + 1, std::memory_order_release);
        consumer_->notify_flag.store(1, std::memory_order_release);
        platform::event_wake_one(&consumer_->notify_flag);
        return value;
    }

    // =================================================================
    // Zero-Copy Loan API (iceoryx2-style)
    // =================================================================

    // WriteLoan - zero-copy write access to a slot
    class WriteLoan {
    public:
        WriteLoan(WriteLoan&& o) noexcept
            : queue_(o.queue_), slot_(o.slot_), write_pos_(o.write_pos_), committed_(o.committed_) {
            o.queue_ = nullptr;
            o.committed_ = true;
        }

        ~WriteLoan() {
            if (queue_ && !committed_) {
                // Rollback: don't advance write_pos
            }
        }

        [[nodiscard]] T* get() { return slot_; }
        [[nodiscard]] const T* get() const { return slot_; }
        T* operator->() { return slot_; }
        T& operator*() { return *slot_; }

        // Commit the loan - publishes the message
        void commit() {
            if (queue_ && !committed_) {
                queue_->producer_->write_pos.store(write_pos_ + 1, std::memory_order_release);
                queue_->producer_->notify_flag.store(1, std::memory_order_release);
                platform::event_wake_one(&queue_->producer_->notify_flag);
                committed_ = true;
            }
        }

        WriteLoan(const WriteLoan&) = delete;
        WriteLoan& operator=(const WriteLoan&) = delete;

    private:
        friend class AsyncSPSCQueue;
        WriteLoan(AsyncSPSCQueue* queue, T* slot, uint64_t write_pos)
            : queue_(queue), slot_(slot), write_pos_(write_pos), committed_(false) {}

        AsyncSPSCQueue* queue_;
        T* slot_;
        uint64_t write_pos_;
        bool committed_;
    };

    // ReadLoan - zero-copy read access to a slot
    class ReadLoan {
    public:
        ReadLoan(ReadLoan&& o) noexcept
            : queue_(o.queue_), slot_(o.slot_), read_pos_(o.read_pos_), released_(o.released_) {
            o.queue_ = nullptr;
            o.released_ = true;
        }

        ~ReadLoan() {
            release();
        }

        [[nodiscard]] const T* get() const { return slot_; }
        const T* operator->() const { return slot_; }
        const T& operator*() const { return *slot_; }

        // Release the loan - allows slot to be reused
        void release() {
            if (queue_ && !released_) {
                queue_->consumer_->read_pos.store(read_pos_ + 1, std::memory_order_release);
                queue_->consumer_->borrow_in_progress.store(0, std::memory_order_release);
                queue_->consumer_->notify_flag.store(1, std::memory_order_release);
                platform::event_wake_one(&queue_->consumer_->notify_flag);
                released_ = true;
            }
        }

        ReadLoan(const ReadLoan&) = delete;
        ReadLoan& operator=(const ReadLoan&) = delete;

    private:
        friend class AsyncSPSCQueue;
        ReadLoan(AsyncSPSCQueue* queue, const T* slot, uint64_t read_pos)
            : queue_(queue), slot_(slot), read_pos_(read_pos), released_(false) {}

        AsyncSPSCQueue* queue_;
        const T* slot_;
        uint64_t read_pos_;
        bool released_;
    };

    // Try to get a write loan (zero-copy send)
    [[nodiscard]] Result<WriteLoan> try_loan() {
        uint64_t write_pos = producer_->write_pos.load(std::memory_order_relaxed);
        uint64_t read_pos = producer_->cached_read_pos;

        if (write_pos - read_pos >= capacity_) {
            read_pos = consumer_->read_pos.load(std::memory_order_acquire);
            producer_->cached_read_pos = read_pos;
            if (write_pos - read_pos >= capacity_) {
                return std::unexpected(Error::QUEUE_FULL);
            }
        }

        T* slot = &data_[write_pos & mask_];
        return WriteLoan(this, slot, write_pos);
    }

    // Try to get a read loan (zero-copy receive)
    [[nodiscard]] Result<ReadLoan> try_borrow() {
        // Check if there's already an outstanding borrow
        uint32_t expected = 0;
        if (!consumer_->borrow_in_progress.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel)) {
            return std::unexpected(Error::INVALID_STATE);  // Already borrowed
        }

        uint64_t read_pos = consumer_->read_pos.load(std::memory_order_relaxed);
        uint64_t write_pos = consumer_->cached_write_pos;

        if (read_pos >= write_pos) {
            write_pos = producer_->write_pos.load(std::memory_order_acquire);
            consumer_->cached_write_pos = write_pos;
            if (read_pos >= write_pos) {
                // No data - release the borrow lock
                consumer_->borrow_in_progress.store(0, std::memory_order_release);
                return std::unexpected(Error::QUEUE_EMPTY);
            }
        }

        const T* slot = &data_[read_pos & mask_];
        return ReadLoan(this, slot, read_pos);
    }

    // =================================================================
    // Batch API (high throughput)
    // =================================================================

    // Try to send a batch of messages (returns number sent)
    [[nodiscard]] size_t try_send_batch(const T* values, size_t count) {
        uint64_t write_pos = producer_->write_pos.load(std::memory_order_relaxed);
        uint64_t read_pos = consumer_->read_pos.load(std::memory_order_acquire);
        producer_->cached_read_pos = read_pos;

        size_t available = capacity_ - (write_pos - read_pos);
        size_t to_send = std::min(count, available);

        for (size_t i = 0; i < to_send; ++i) {
            data_[(write_pos + i) & mask_] = values[i];
        }

        if (to_send > 0) {
            producer_->write_pos.store(write_pos + to_send, std::memory_order_release);
            producer_->notify_flag.store(1, std::memory_order_release);
            platform::event_wake_one(&producer_->notify_flag);
        }

        return to_send;
    }

    // Try to receive a batch of messages (returns number received)
    [[nodiscard]] size_t try_receive_batch(T* values, size_t max_count) {
        uint64_t read_pos = consumer_->read_pos.load(std::memory_order_relaxed);
        uint64_t write_pos = producer_->write_pos.load(std::memory_order_acquire);
        consumer_->cached_write_pos = write_pos;

        size_t available = write_pos - read_pos;
        size_t to_receive = std::min(max_count, available);

        for (size_t i = 0; i < to_receive; ++i) {
            values[i] = data_[(read_pos + i) & mask_];
        }

        if (to_receive > 0) {
            consumer_->read_pos.store(read_pos + to_receive, std::memory_order_release);
            consumer_->notify_flag.store(1, std::memory_order_release);
            platform::event_wake_one(&consumer_->notify_flag);
        }

        return to_receive;
    }

    // Get direct pointer to data buffer (for zero-copy batch operations)
    [[nodiscard]] T* data_ptr() { return data_; }
    [[nodiscard]] const T* data_ptr() const { return data_; }
    [[nodiscard]] uint64_t mask() const { return mask_; }

    // =================================================================
    // Backpressure Handling APIs
    // =================================================================

    // Backpressure strategy enum
    enum class BackpressureStrategy {
        SPIN,           // Tight spin loop (lowest latency, highest CPU)
        YIELD,          // yield() between attempts (good balance)
        PAUSE,          // _mm_pause() for x86 (CPU-friendly spin)
        EXPONENTIAL     // Exponential backoff (best for sustained pressure)
    };

    // 1. Blocking send - waits until space available
    void send_blocking(const T& value, BackpressureStrategy strategy = BackpressureStrategy::YIELD) {
        uint32_t spin_count = 0;
        while (true) {
            if (auto r = try_send(value); r) {
                return;
            }
            apply_backoff(strategy, spin_count++);
        }
    }

    // 2. Blocking batch send - sends ALL messages, waiting as needed
    void send_batch_blocking(const T* values, size_t count,
                             BackpressureStrategy strategy = BackpressureStrategy::YIELD) {
        size_t sent = 0;
        uint32_t spin_count = 0;
        while (sent < count) {
            size_t batch_sent = try_send_batch(values + sent, count - sent);
            sent += batch_sent;
            if (sent < count) {
                apply_backoff(strategy, spin_count++);
            } else {
                spin_count = 0;  // Reset on success
            }
        }
    }

    // 3. Blocking loan - waits until slot available
    WriteLoan loan_blocking(BackpressureStrategy strategy = BackpressureStrategy::YIELD) {
        uint32_t spin_count = 0;
        while (true) {
            if (auto r = try_loan(); r) {
                return std::move(*r);
            }
            apply_backoff(strategy, spin_count++);
        }
    }

    // =================================================================
    // Adaptive Rate Limiting
    // =================================================================

    // Get current queue fill level (0.0 - 1.0)
    [[nodiscard]] double fill_level() const {
        uint64_t write_pos = producer_->write_pos.load(std::memory_order_relaxed);
        uint64_t read_pos = consumer_->read_pos.load(std::memory_order_relaxed);
        uint64_t used = write_pos - read_pos;
        return static_cast<double>(used) / static_cast<double>(capacity_);
    }

    // Get available slots
    [[nodiscard]] size_t available_slots() const {
        uint64_t write_pos = producer_->write_pos.load(std::memory_order_relaxed);
        uint64_t read_pos = consumer_->read_pos.load(std::memory_order_relaxed);
        return capacity_ - (write_pos - read_pos);
    }

    // Adaptive batch size based on fill level
    [[nodiscard]] size_t adaptive_batch_size(size_t max_batch) const {
        double fill = fill_level();
        if (fill < 0.25) return max_batch;           // Queue mostly empty - full speed
        if (fill < 0.50) return max_batch * 3 / 4;   // Quarter full - slight reduction
        if (fill < 0.75) return max_batch / 2;       // Half full - reduce by half
        if (fill < 0.90) return max_batch / 4;       // Nearly full - quarter speed
        return 1;                                     // Critical - single messages
    }

    // Adaptive delay based on fill level (returns nanoseconds to wait)
    [[nodiscard]] uint64_t adaptive_delay_ns() const {
        double fill = fill_level();
        if (fill < 0.50) return 0;                   // No delay needed
        if (fill < 0.75) return 100;                 // 100ns
        if (fill < 0.90) return 1000;                // 1μs
        if (fill < 0.95) return 10000;               // 10μs
        return 100000;                               // 100μs - critical
    }

    // =================================================================
    // Credit-Based Flow Control
    // =================================================================

    // Credit system - producer requests credits from consumer
    // Credits represent slots that consumer has freed

    // Request credits (returns number of slots consumer has freed)
    [[nodiscard]] size_t request_credits(size_t max_credits) {
        uint64_t write_pos = producer_->write_pos.load(std::memory_order_relaxed);
        uint64_t read_pos = consumer_->read_pos.load(std::memory_order_acquire);
        producer_->cached_read_pos = read_pos;

        size_t available = capacity_ - (write_pos - read_pos);
        return std::min(max_credits, available);
    }

    // Send with credits - only sends up to available credits
    [[nodiscard]] size_t send_with_credits(const T* values, size_t count, size_t& credits) {
        size_t to_send = std::min(count, credits);
        if (to_send == 0) {
            // Refresh credits
            credits = request_credits(count);
            to_send = std::min(count, credits);
        }

        if (to_send > 0) {
            size_t sent = try_send_batch(values, to_send);
            credits -= sent;
            return sent;
        }
        return 0;
    }

    // High-water mark check (for flow control decisions)
    [[nodiscard]] bool is_high_water(double threshold = 0.80) const {
        return fill_level() >= threshold;
    }

    // Low-water mark check (safe to resume sending)
    [[nodiscard]] bool is_low_water(double threshold = 0.25) const {
        return fill_level() <= threshold;
    }

private:
    // Apply backoff strategy
    static void apply_backoff(BackpressureStrategy strategy, uint32_t spin_count) {
        switch (strategy) {
            case BackpressureStrategy::SPIN:
                // Pure spin - do nothing (lowest latency, highest CPU)
                break;
            case BackpressureStrategy::YIELD:
                std::this_thread::yield();
                break;
            case BackpressureStrategy::PAUSE:
                #if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
                #elif defined(__aarch64__)
                    asm volatile("yield" ::: "memory");
                #else
                    std::this_thread::yield();
                #endif
                break;
            case BackpressureStrategy::EXPONENTIAL:
                if (spin_count < 10) {
                    // First 10 attempts: just pause
                    #if defined(__x86_64__) || defined(_M_X64)
                        __builtin_ia32_pause();
                    #elif defined(__aarch64__)
                        asm volatile("yield" ::: "memory");
                    #endif
                } else if (spin_count < 20) {
                    // Next 10: yield
                    std::this_thread::yield();
                } else {
                    // Beyond: sleep with exponential backoff
                    auto delay = std::chrono::nanoseconds{
                        std::min(1000000UL, 100UL << std::min(spin_count - 20, 10U))
                    };
                    std::this_thread::sleep_for(delay);
                }
                break;
        }
    }

public:
    // Async operations
    asyncio::task::Task<void> send(T value) {
        while (true) {
            if (auto r = try_send(value); r) {
                co_return;
            }
            co_await asyncio::sleep(std::chrono::milliseconds{1});
        }
    }

    asyncio::task::Task<T> receive() {
        while (true) {
            if (auto r = try_receive(); r) {
                co_return *r;
            }
            co_await asyncio::sleep(std::chrono::milliseconds{1});
        }
    }

    asyncio::task::Task<Result<T>> receive_timeout(std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto r = try_receive(); r) {
                co_return r;
            }
            co_await asyncio::sleep(std::chrono::milliseconds{1});
        }
        co_return std::unexpected(Error::TIMEOUT);
    }

    [[nodiscard]] size_t capacity() const { return capacity_; }

    AsyncSPSCQueue(AsyncSPSCQueue&& o) noexcept
        : name_(std::move(o.name_)), ptr_(o.ptr_), size_(o.size_), owner_(o.owner_),
          header_(o.header_), producer_(o.producer_), consumer_(o.consumer_),
          data_(o.data_), capacity_(o.capacity_), mask_(o.mask_) { o.ptr_ = nullptr; }

    AsyncSPSCQueue(const AsyncSPSCQueue&) = delete;
    AsyncSPSCQueue& operator=(const AsyncSPSCQueue&) = delete;

private:
    AsyncSPSCQueue(std::string name, void* ptr, size_t size, bool owner,
                   Header* header, ProducerData* producer, ConsumerData* consumer,
                   T* data, uint64_t capacity)
        : name_(std::move(name)), ptr_(ptr), size_(size), owner_(owner),
          header_(header), producer_(producer), consumer_(consumer),
          data_(data), capacity_(capacity), mask_(capacity - 1) {}

    std::string name_;
    void* ptr_{nullptr};
    size_t size_{0};
    bool owner_{false};
    Header* header_{nullptr};
    ProducerData* producer_{nullptr};
    ConsumerData* consumer_{nullptr};
    T* data_{nullptr};
    uint64_t capacity_{0};
    uint64_t mask_{0};
};

// =============================================================================
// AsyncSPMCQueue - Single Producer, Multiple Consumers (Pub/Sub)
// =============================================================================

template<ShmCompatible T>
class AsyncSPMCQueue {
public:
    struct alignas(CACHE_LINE_SIZE) Header {
        uint64_t magic{0};
        uint64_t capacity{0};
        std::atomic<uint32_t> subscriber_count{0};
    };

    struct alignas(CACHE_LINE_SIZE) ProducerData {
        std::atomic<uint64_t> write_pos{0};
        std::atomic<uint64_t> min_read_pos{0};
    };

    struct alignas(CACHE_LINE_SIZE) SubscriberData {
        std::atomic<uint64_t> read_pos{0};
        std::atomic<uint32_t> active{0};
        uint64_t cached_write_pos{0};
    };

    class Publisher {
    public:
        [[nodiscard]] Result<void> try_publish(const T& value) {
            uint64_t write_pos = queue_->producer_->write_pos.load(std::memory_order_relaxed);
            uint64_t min_read = queue_->producer_->min_read_pos.load(std::memory_order_acquire);

            if (write_pos - min_read >= queue_->capacity_) {
                // Recalculate min read position
                min_read = UINT64_MAX;
                for (size_t i = 0; i < MAX_SUBSCRIBERS; ++i) {
                    if (queue_->subscribers_[i].active.load(std::memory_order_acquire)) {
                        uint64_t rp = queue_->subscribers_[i].read_pos.load(std::memory_order_acquire);
                        min_read = std::min(min_read, rp);
                    }
                }
                if (min_read == UINT64_MAX) min_read = write_pos;
                queue_->producer_->min_read_pos.store(min_read, std::memory_order_release);

                if (write_pos - min_read >= queue_->capacity_) {
                    return std::unexpected(Error::QUEUE_FULL);
                }
            }

            queue_->data_[write_pos & queue_->mask_] = value;
            queue_->producer_->write_pos.store(write_pos + 1, std::memory_order_release);
            return {};
        }

        asyncio::task::Task<void> publish(T value) {
            while (true) {
                if (auto r = try_publish(value); r) {
                    co_return;
                }
                co_await asyncio::sleep(std::chrono::milliseconds{1});
            }
        }

        [[nodiscard]] uint32_t subscriber_count() const {
            return queue_->header_->subscriber_count.load(std::memory_order_acquire);
        }

        Publisher(Publisher&& o) noexcept : queue_(o.queue_) { o.queue_ = nullptr; }
        Publisher(const Publisher&) = delete;
        ~Publisher() = default;

    private:
        friend class AsyncSPMCQueue;
        explicit Publisher(AsyncSPMCQueue* q) : queue_(q) {}
        AsyncSPMCQueue* queue_{nullptr};
    };

    class Subscriber {
    public:
        [[nodiscard]] Result<T> try_receive() {
            uint64_t read_pos = data_->read_pos.load(std::memory_order_relaxed);
            uint64_t write_pos = data_->cached_write_pos;

            if (read_pos >= write_pos) {
                write_pos = queue_->producer_->write_pos.load(std::memory_order_acquire);
                data_->cached_write_pos = write_pos;
                if (read_pos >= write_pos) {
                    return std::unexpected(Error::QUEUE_EMPTY);
                }
            }

            T value = queue_->data_[read_pos & queue_->mask_];
            data_->read_pos.store(read_pos + 1, std::memory_order_release);
            return value;
        }

        asyncio::task::Task<T> receive() {
            while (true) {
                if (auto r = try_receive(); r) {
                    co_return *r;
                }
                co_await asyncio::sleep(std::chrono::milliseconds{1});
            }
        }

        ~Subscriber() {
            if (data_) {
                data_->active.store(0, std::memory_order_release);
                queue_->header_->subscriber_count.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        Subscriber(Subscriber&& o) noexcept : queue_(o.queue_), data_(o.data_), id_(o.id_) {
            o.data_ = nullptr;
        }
        Subscriber(const Subscriber&) = delete;

    private:
        friend class AsyncSPMCQueue;
        Subscriber(AsyncSPMCQueue* q, SubscriberData* d, int id) : queue_(q), data_(d), id_(id) {}
        AsyncSPMCQueue* queue_{nullptr};
        SubscriberData* data_{nullptr};
        int id_{-1};
    };

    static AsyncSPMCQueue create(const std::string& name, size_t capacity) {
        capacity = std::bit_ceil(capacity);

        size_t total_size = sizeof(Header) + sizeof(ProducerData) +
                           sizeof(SubscriberData) * MAX_SUBSCRIBERS + sizeof(T) * capacity;

        void* ptr = platform::shm_create(name, total_size);
        if (!ptr) throw std::runtime_error("Failed to create SPMC queue: " + name);

        std::memset(ptr, 0, total_size);

        char* p = static_cast<char*>(ptr);
        auto* header = new (p) Header{};
        auto* producer = new (p + sizeof(Header)) ProducerData{};
        auto* subscribers = reinterpret_cast<SubscriberData*>(p + sizeof(Header) + sizeof(ProducerData));
        auto* data = reinterpret_cast<T*>(p + sizeof(Header) + sizeof(ProducerData) +
                                          sizeof(SubscriberData) * MAX_SUBSCRIBERS);

        header->capacity = capacity;
        std::atomic_thread_fence(std::memory_order_release);
        header->magic = MAGIC_SPMC;

        return AsyncSPMCQueue(name, ptr, total_size, true, header, producer, subscribers, data, capacity);
    }

    Publisher publisher() {
        if (!owner_) throw std::runtime_error("Only creator can be publisher");
        return Publisher(this);
    }

    [[nodiscard]] Result<Subscriber> subscribe() {
        for (size_t i = 0; i < MAX_SUBSCRIBERS; ++i) {
            uint32_t expected = 0;
            if (subscribers_[i].active.compare_exchange_strong(expected, 1,
                    std::memory_order_acq_rel)) {
                subscribers_[i].read_pos.store(
                    producer_->write_pos.load(std::memory_order_acquire),
                    std::memory_order_release);
                header_->subscriber_count.fetch_add(1, std::memory_order_acq_rel);
                return Subscriber(this, &subscribers_[i], static_cast<int>(i));
            }
        }
        return std::unexpected(Error::TOO_MANY_SUBSCRIBERS);
    }

    ~AsyncSPMCQueue() {
        if (ptr_) {
            platform::shm_close(ptr_, size_);
            if (owner_) platform::shm_unlink(name_);
        }
    }

    [[nodiscard]] size_t capacity() const { return capacity_; }

    AsyncSPMCQueue(AsyncSPMCQueue&& o) noexcept
        : name_(std::move(o.name_)), ptr_(o.ptr_), size_(o.size_), owner_(o.owner_),
          header_(o.header_), producer_(o.producer_), subscribers_(o.subscribers_),
          data_(o.data_), capacity_(o.capacity_), mask_(o.mask_) { o.ptr_ = nullptr; }

    AsyncSPMCQueue(const AsyncSPMCQueue&) = delete;
    AsyncSPMCQueue& operator=(const AsyncSPMCQueue&) = delete;

private:
    AsyncSPMCQueue(std::string name, void* ptr, size_t size, bool owner,
                   Header* header, ProducerData* producer, SubscriberData* subscribers,
                   T* data, uint64_t capacity)
        : name_(std::move(name)), ptr_(ptr), size_(size), owner_(owner),
          header_(header), producer_(producer), subscribers_(subscribers),
          data_(data), capacity_(capacity), mask_(capacity - 1) {}

    std::string name_;
    void* ptr_{nullptr};
    size_t size_{0};
    bool owner_{false};
    Header* header_{nullptr};
    ProducerData* producer_{nullptr};
    SubscriberData* subscribers_{nullptr};
    T* data_{nullptr};
    uint64_t capacity_{0};
    uint64_t mask_{0};
};

// =============================================================================
// AsyncMPSCQueue - Multiple Producers, Single Consumer
// =============================================================================

template<ShmCompatible T>
class AsyncMPSCQueue {
public:
    struct alignas(CACHE_LINE_SIZE) Header {
        uint64_t magic{0};
        uint64_t capacity{0};
    };

    struct alignas(CACHE_LINE_SIZE) ProducerShared {
        std::atomic<uint64_t> write_claim{0};
        std::atomic<uint64_t> write_committed{0};
    };

    struct alignas(CACHE_LINE_SIZE) ConsumerData {
        std::atomic<uint64_t> read_pos{0};
        uint64_t cached_committed{0};
    };

    struct alignas(CACHE_LINE_SIZE) Slot {
        std::atomic<uint32_t> ready{0};
        T data;
    };

    class Producer {
    public:
        [[nodiscard]] Result<void> try_send(const T& value) {
            uint64_t claimed = queue_->producer_->write_claim.fetch_add(1, std::memory_order_acq_rel);
            uint64_t read_pos = queue_->consumer_->read_pos.load(std::memory_order_acquire);

            if (claimed - read_pos >= queue_->capacity_) {
                // Queue full - unclaim
                queue_->producer_->write_claim.fetch_sub(1, std::memory_order_acq_rel);
                return std::unexpected(Error::QUEUE_FULL);
            }

            Slot* slot = &queue_->slots_[claimed & queue_->mask_];
            slot->data = value;
            slot->ready.store(1, std::memory_order_release);

            // Try to advance committed position
            uint64_t expected = claimed;
            while (!queue_->producer_->write_committed.compare_exchange_weak(
                    expected, claimed + 1, std::memory_order_acq_rel)) {
                if (expected > claimed) break;  // Someone else advanced it
                expected = claimed;
                std::this_thread::yield();
            }

            return {};
        }

        asyncio::task::Task<void> send(T value) {
            while (true) {
                if (auto r = try_send(value); r) {
                    co_return;
                }
                co_await asyncio::sleep(std::chrono::milliseconds{1});
            }
        }

        Producer(Producer&& o) noexcept : queue_(o.queue_) { o.queue_ = nullptr; }
        Producer(const Producer&) = delete;
        ~Producer() = default;

    private:
        friend class AsyncMPSCQueue;
        explicit Producer(AsyncMPSCQueue* q) : queue_(q) {}
        AsyncMPSCQueue* queue_{nullptr};
    };

    static AsyncMPSCQueue create(const std::string& name, size_t capacity) {
        capacity = std::bit_ceil(capacity);

        size_t total_size = sizeof(Header) + sizeof(ProducerShared) +
                           sizeof(ConsumerData) + sizeof(Slot) * capacity;

        void* ptr = platform::shm_create(name, total_size);
        if (!ptr) throw std::runtime_error("Failed to create MPSC queue: " + name);

        std::memset(ptr, 0, total_size);

        char* p = static_cast<char*>(ptr);
        auto* header = new (p) Header{};
        auto* producer = new (p + sizeof(Header)) ProducerShared{};
        auto* consumer = new (p + sizeof(Header) + sizeof(ProducerShared)) ConsumerData{};
        auto* slots = reinterpret_cast<Slot*>(p + sizeof(Header) + sizeof(ProducerShared) + sizeof(ConsumerData));

        header->capacity = capacity;
        std::atomic_thread_fence(std::memory_order_release);
        header->magic = MAGIC_MPSC;

        return AsyncMPSCQueue(name, ptr, total_size, true, header, producer, consumer, slots, capacity);
    }

    Producer producer() {
        return Producer(this);
    }

    [[nodiscard]] Result<T> try_receive() {
        uint64_t read_pos = consumer_->read_pos.load(std::memory_order_relaxed);
        uint64_t committed = consumer_->cached_committed;

        if (read_pos >= committed) {
            committed = producer_->write_committed.load(std::memory_order_acquire);
            consumer_->cached_committed = committed;
            if (read_pos >= committed) {
                return std::unexpected(Error::QUEUE_EMPTY);
            }
        }

        Slot* slot = &slots_[read_pos & mask_];

        // Wait for slot to be ready
        while (slot->ready.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }

        T value = slot->data;
        slot->ready.store(0, std::memory_order_release);
        consumer_->read_pos.store(read_pos + 1, std::memory_order_release);
        return value;
    }

    asyncio::task::Task<T> receive() {
        while (true) {
            if (auto r = try_receive(); r) {
                co_return *r;
            }
            co_await asyncio::sleep(std::chrono::milliseconds{1});
        }
    }

    ~AsyncMPSCQueue() {
        if (ptr_) {
            platform::shm_close(ptr_, size_);
            if (owner_) platform::shm_unlink(name_);
        }
    }

    [[nodiscard]] size_t capacity() const { return capacity_; }

    AsyncMPSCQueue(AsyncMPSCQueue&& o) noexcept
        : name_(std::move(o.name_)), ptr_(o.ptr_), size_(o.size_), owner_(o.owner_),
          header_(o.header_), producer_(o.producer_), consumer_(o.consumer_),
          slots_(o.slots_), capacity_(o.capacity_), mask_(o.mask_) { o.ptr_ = nullptr; }

    AsyncMPSCQueue(const AsyncMPSCQueue&) = delete;
    AsyncMPSCQueue& operator=(const AsyncMPSCQueue&) = delete;

private:
    AsyncMPSCQueue(std::string name, void* ptr, size_t size, bool owner,
                   Header* header, ProducerShared* producer, ConsumerData* consumer,
                   Slot* slots, uint64_t capacity)
        : name_(std::move(name)), ptr_(ptr), size_(size), owner_(owner),
          header_(header), producer_(producer), consumer_(consumer),
          slots_(slots), capacity_(capacity), mask_(capacity - 1) {}

    std::string name_;
    void* ptr_{nullptr};
    size_t size_{0};
    bool owner_{false};
    Header* header_{nullptr};
    ProducerShared* producer_{nullptr};
    ConsumerData* consumer_{nullptr};
    Slot* slots_{nullptr};
    uint64_t capacity_{0};
    uint64_t mask_{0};
};

// =============================================================================
// AsyncRpcChannel - Request/Response Pattern
// =============================================================================

template<ShmCompatible Request, ShmCompatible Response>
class AsyncRpcChannel {
public:
    struct alignas(CACHE_LINE_SIZE) RequestSlot {
        uint64_t request_id;
        uint64_t timestamp_ns;
        Request payload;
    };

    struct alignas(CACHE_LINE_SIZE) ResponseSlot {
        uint64_t request_id;
        uint64_t timestamp_ns;
        Response payload;
    };

    class Client {
    public:
        // Async RPC call with timeout
        asyncio::task::Task<Result<Response>> call(const Request& req,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
            uint64_t req_id = next_id_++;

            RequestSlot slot;
            slot.request_id = req_id;
            slot.timestamp_ns = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            slot.payload = req;

            // Send request
            if (auto r = request_queue_->try_send(slot); !r) {
                co_return std::unexpected(r.error());
            }

            // Wait for response
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (auto r = response_queue_->try_receive(); r) {
                    if (r->request_id == req_id) {
                        co_return r->payload;
                    }
                    // Wrong ID, keep waiting
                }
                co_await asyncio::sleep(std::chrono::milliseconds{1});
            }

            co_return std::unexpected(Error::TIMEOUT);
        }

        Client(Client&& o) noexcept
            : request_queue_(o.request_queue_), response_queue_(o.response_queue_),
              next_id_(o.next_id_.load()) { o.request_queue_ = nullptr; }
        Client(const Client&) = delete;
        ~Client() = default;

    private:
        friend class AsyncRpcChannel;
        Client(AsyncSPSCQueue<RequestSlot>* req, AsyncSPSCQueue<ResponseSlot>* resp)
            : request_queue_(req), response_queue_(resp) {}

        AsyncSPSCQueue<RequestSlot>* request_queue_{nullptr};
        AsyncSPSCQueue<ResponseSlot>* response_queue_{nullptr};
        std::atomic<uint64_t> next_id_{1};
    };

    class Server {
    public:
        // Async receive request
        asyncio::task::Task<Result<std::pair<uint64_t, Request>>> receive_request() {
            auto r = co_await request_queue_->receive();
            co_return std::pair{r.request_id, r.payload};
        }

        // Send response
        [[nodiscard]] Result<void> send_response(uint64_t request_id, const Response& resp) {
            ResponseSlot slot;
            slot.request_id = request_id;
            slot.timestamp_ns = static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            slot.payload = resp;
            return response_queue_->try_send(slot);
        }

        // Handle one request with callback
        template<typename Handler>
            requires std::invocable<Handler, const Request&> &&
                     std::same_as<std::invoke_result_t<Handler, const Request&>, Response>
        asyncio::task::Task<Result<void>> handle_one(Handler&& handler) {
            auto req_result = request_queue_->try_receive();
            if (!req_result) {
                co_return std::unexpected(req_result.error());
            }

            Response resp = handler(req_result->payload);
            co_return send_response(req_result->request_id, resp);
        }

        Server(Server&& o) noexcept
            : request_queue_(o.request_queue_), response_queue_(o.response_queue_) {
            o.request_queue_ = nullptr;
        }
        Server(const Server&) = delete;
        ~Server() = default;

    private:
        friend class AsyncRpcChannel;
        Server(AsyncSPSCQueue<RequestSlot>* req, AsyncSPSCQueue<ResponseSlot>* resp)
            : request_queue_(req), response_queue_(resp) {}

        AsyncSPSCQueue<RequestSlot>* request_queue_{nullptr};
        AsyncSPSCQueue<ResponseSlot>* response_queue_{nullptr};
    };

    struct CreateResult {
        Client client;
        Server server;
    };

    static CreateResult create(const std::string& name, size_t capacity = 256) {
        // Store queues in static storage (simplified - in production use shared_ptr)
        static std::vector<std::unique_ptr<AsyncSPSCQueue<RequestSlot>>> req_queues;
        static std::vector<std::unique_ptr<AsyncSPSCQueue<ResponseSlot>>> resp_queues;

        auto req_queue = std::make_unique<AsyncSPSCQueue<RequestSlot>>(
            AsyncSPSCQueue<RequestSlot>::create(name + "_req", capacity));
        auto resp_queue = std::make_unique<AsyncSPSCQueue<ResponseSlot>>(
            AsyncSPSCQueue<ResponseSlot>::create(name + "_resp", capacity));

        auto* req_ptr = req_queue.get();
        auto* resp_ptr = resp_queue.get();

        req_queues.push_back(std::move(req_queue));
        resp_queues.push_back(std::move(resp_queue));

        return CreateResult{Client(req_ptr, resp_ptr), Server(req_ptr, resp_ptr)};
    }
};

// =============================================================================
// AsyncMemoryPool - Lock-free Memory Pool with Loan API
// =============================================================================

class AsyncMemoryPool {
public:
    static constexpr size_t NUM_SIZE_CLASSES = 11;
    static constexpr std::array<size_t, NUM_SIZE_CLASSES> SIZE_CLASSES = {
        64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
    };

    struct alignas(CACHE_LINE_SIZE) BlockHeader {
        std::atomic<BlockHeader*> next;
        uint32_t size_class;
        uint32_t magic;
    };

    struct alignas(CACHE_LINE_SIZE) FreeList {
        std::atomic<BlockHeader*> head{nullptr};
        std::atomic<uint64_t> alloc_count{0};
        std::atomic<uint64_t> free_count{0};
    };

    struct alignas(CACHE_LINE_SIZE) Header {
        uint64_t magic{0};
        uint64_t total_size{0};
        FreeList free_lists[NUM_SIZE_CLASSES];
    };

    // RAII wrapper for allocated memory
    template<typename T>
    class Loan {
    public:
        T* get() { return ptr_; }
        T* operator->() { return ptr_; }
        T& operator*() { return *ptr_; }

        explicit operator bool() const { return ptr_ != nullptr; }

        ~Loan() {
            if (ptr_ && pool_) {
                pool_->deallocate(ptr_);
            }
        }

        Loan(Loan&& o) noexcept : pool_(o.pool_), ptr_(o.ptr_) {
            o.ptr_ = nullptr;
        }
        Loan(const Loan&) = delete;
        Loan& operator=(const Loan&) = delete;

    private:
        friend class AsyncMemoryPool;
        Loan(AsyncMemoryPool* pool, T* ptr) : pool_(pool), ptr_(ptr) {}
        AsyncMemoryPool* pool_{nullptr};
        T* ptr_{nullptr};
    };

    static AsyncMemoryPool create(const std::string& name, size_t total_size) {
        void* ptr = platform::shm_create(name, total_size);
        if (!ptr) throw std::runtime_error("Failed to create memory pool: " + name);

        std::memset(ptr, 0, total_size);

        auto* header = new (ptr) Header{};
        header->total_size = total_size;

        // Initialize free lists with pre-allocated blocks
        char* pool_start = static_cast<char*>(ptr) + sizeof(Header);
        size_t pool_size = total_size - sizeof(Header);
        size_t offset = 0;

        // Distribute pool among size classes
        for (size_t sc = 0; sc < NUM_SIZE_CLASSES && offset < pool_size; ++sc) {
            size_t block_size = SIZE_CLASSES[sc] + sizeof(BlockHeader);
            size_t num_blocks = (pool_size / NUM_SIZE_CLASSES) / block_size;

            for (size_t i = 0; i < num_blocks && offset + block_size <= pool_size; ++i) {
                auto* block = reinterpret_cast<BlockHeader*>(pool_start + offset);
                block->size_class = static_cast<uint32_t>(sc);
                block->magic = 0xBEEF;

                // Push to free list
                block->next.store(header->free_lists[sc].head.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
                header->free_lists[sc].head.store(block, std::memory_order_release);

                offset += block_size;
            }
        }

        std::atomic_thread_fence(std::memory_order_release);
        header->magic = MAGIC_POOL;

        return AsyncMemoryPool(name, ptr, total_size, true, header);
    }

    void* allocate(size_t size) {
        size_t sc = size_class_for(size + sizeof(BlockHeader));
        if (sc >= NUM_SIZE_CLASSES) return nullptr;

        // Try this size class and larger
        for (; sc < NUM_SIZE_CLASSES; ++sc) {
            BlockHeader* block = header_->free_lists[sc].head.load(std::memory_order_acquire);
            while (block) {
                if (header_->free_lists[sc].head.compare_exchange_weak(
                        block, block->next.load(std::memory_order_relaxed),
                        std::memory_order_acq_rel)) {
                    header_->free_lists[sc].alloc_count.fetch_add(1, std::memory_order_relaxed);
                    return reinterpret_cast<char*>(block) + sizeof(BlockHeader);
                }
            }
        }
        return nullptr;
    }

    void deallocate(void* ptr) {
        if (!ptr) return;

        auto* block = reinterpret_cast<BlockHeader*>(
            static_cast<char*>(ptr) - sizeof(BlockHeader));
        if (block->magic != 0xBEEF) return;  // Invalid block

        size_t sc = block->size_class;
        if (sc >= NUM_SIZE_CLASSES) return;

        // Push back to free list
        BlockHeader* old_head = header_->free_lists[sc].head.load(std::memory_order_relaxed);
        do {
            block->next.store(old_head, std::memory_order_relaxed);
        } while (!header_->free_lists[sc].head.compare_exchange_weak(
                    old_head, block, std::memory_order_acq_rel));

        header_->free_lists[sc].free_count.fetch_add(1, std::memory_order_relaxed);
    }

    template<typename T, typename... Args>
    [[nodiscard]] Loan<T> loan(Args&&... args) {
        void* ptr = allocate(sizeof(T));
        if (!ptr) return Loan<T>(nullptr, nullptr);
        T* obj = new (ptr) T(std::forward<Args>(args)...);
        return Loan<T>(this, obj);
    }

    // Async allocate - waits for memory if pool exhausted
    asyncio::task::Task<void*> allocate_async(size_t size) {
        while (true) {
            void* ptr = allocate(size);
            if (ptr) co_return ptr;
            co_await asyncio::sleep(std::chrono::milliseconds{1});
        }
    }

    ~AsyncMemoryPool() {
        if (ptr_) {
            platform::shm_close(ptr_, size_);
            if (owner_) platform::shm_unlink(name_);
        }
    }

    AsyncMemoryPool(AsyncMemoryPool&& o) noexcept
        : name_(std::move(o.name_)), ptr_(o.ptr_), size_(o.size_),
          owner_(o.owner_), header_(o.header_) { o.ptr_ = nullptr; }

    AsyncMemoryPool(const AsyncMemoryPool&) = delete;
    AsyncMemoryPool& operator=(const AsyncMemoryPool&) = delete;

private:
    AsyncMemoryPool(std::string name, void* ptr, size_t size, bool owner, Header* header)
        : name_(std::move(name)), ptr_(ptr), size_(size), owner_(owner), header_(header) {}

    static constexpr size_t size_class_for(size_t size) {
        for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
            if (SIZE_CLASSES[i] >= size) return i;
        }
        return NUM_SIZE_CLASSES;
    }

    std::string name_;
    void* ptr_{nullptr};
    size_t size_{0};
    bool owner_{false};
    Header* header_{nullptr};
};

// =============================================================================
// Helper: Batched Message for high-throughput scenarios
// =============================================================================

template<ShmCompatible T, size_t MaxBatchSize = 64>
struct alignas(CACHE_LINE_SIZE) BatchedMessage {
    uint64_t batch_sequence;
    uint64_t timestamp_ns;
    uint32_t message_count;
    T messages[MaxBatchSize];
};

// =============================================================================
// Heartbeat Monitor - Dead Endpoint Detection
// =============================================================================

class HeartbeatMonitor {
public:
    static constexpr uint64_t MAGIC_HB = 0xA510484200001ULL;
    static constexpr uint64_t DEFAULT_HEARTBEAT_INTERVAL_MS = 100;
    static constexpr uint64_t DEFAULT_TIMEOUT_MS = 500;

    struct EndpointInfo {
        std::atomic<uint64_t> last_heartbeat_ns{0};
        std::atomic<uint32_t> pid{0};
        std::atomic<uint32_t> status{0};  // 0=inactive, 1=active, 2=dead
        char name[64]{};
    };

    struct alignas(CACHE_LINE_SIZE) Header {
        uint64_t magic;
        std::atomic<uint32_t> endpoint_count{0};
        std::atomic<uint32_t> version{0};
        uint64_t heartbeat_interval_ns;
        uint64_t timeout_ns;
        char _pad[64 - 32];
    };

    static constexpr size_t MAX_ENDPOINTS = 64;

private:
    std::string name_;
    void* ptr_{nullptr};
    size_t size_{0};
    bool owner_{false};
    Header* header_{nullptr};
    EndpointInfo* endpoints_{nullptr};
    uint32_t my_endpoint_id_{UINT32_MAX};
    std::atomic<bool> running_{false};

public:
    static HeartbeatMonitor create(const std::string& name,
                                   uint64_t heartbeat_interval_ms = DEFAULT_HEARTBEAT_INTERVAL_MS,
                                   uint64_t timeout_ms = DEFAULT_TIMEOUT_MS) {
        size_t size = sizeof(Header) + sizeof(EndpointInfo) * MAX_ENDPOINTS;
        void* ptr = platform::shm_create(name, size);
        if (!ptr) throw std::runtime_error("Failed to create heartbeat monitor");

        auto* header = static_cast<Header*>(ptr);
        header->magic = MAGIC_HB;
        header->endpoint_count.store(0, std::memory_order_release);
        header->version.store(0, std::memory_order_release);
        header->heartbeat_interval_ns = heartbeat_interval_ms * 1'000'000;
        header->timeout_ns = timeout_ms * 1'000'000;

        auto* endpoints = reinterpret_cast<EndpointInfo*>(
            static_cast<char*>(ptr) + sizeof(Header));
        for (size_t i = 0; i < MAX_ENDPOINTS; ++i) {
            endpoints[i].status.store(0, std::memory_order_release);
        }

        return HeartbeatMonitor(name, ptr, size, true, header, endpoints);
    }

    static HeartbeatMonitor open(const std::string& name) {
        size_t size = 0;
        void* ptr = platform::shm_open_existing(name, size);
        if (!ptr) throw std::runtime_error("Failed to open heartbeat monitor");

        auto* header = static_cast<Header*>(ptr);
        if (header->magic != MAGIC_HB) {
            platform::shm_close(ptr, size);
            throw std::runtime_error("Invalid heartbeat monitor magic");
        }

        auto* endpoints = reinterpret_cast<EndpointInfo*>(
            static_cast<char*>(ptr) + sizeof(Header));
        return HeartbeatMonitor(name, ptr, size, false, header, endpoints);
    }

    // Register this process as an endpoint
    uint32_t register_endpoint(const std::string& endpoint_name) {
        uint32_t id = header_->endpoint_count.fetch_add(1, std::memory_order_acq_rel);
        if (id >= MAX_ENDPOINTS) {
            header_->endpoint_count.fetch_sub(1, std::memory_order_relaxed);
            return UINT32_MAX;
        }

        auto now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        endpoints_[id].last_heartbeat_ns.store(now_ns, std::memory_order_release);
        endpoints_[id].pid.store(static_cast<uint32_t>(getpid()), std::memory_order_release);
        endpoints_[id].status.store(1, std::memory_order_release);
        std::strncpy(endpoints_[id].name, endpoint_name.c_str(), sizeof(endpoints_[id].name) - 1);

        my_endpoint_id_ = id;
        return id;
    }

    // Send heartbeat (call periodically)
    void heartbeat() {
        if (my_endpoint_id_ == UINT32_MAX) return;
        auto now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        endpoints_[my_endpoint_id_].last_heartbeat_ns.store(now_ns, std::memory_order_release);
    }

    // Check if an endpoint is alive
    [[nodiscard]] bool is_alive(uint32_t endpoint_id) const {
        if (endpoint_id >= MAX_ENDPOINTS) return false;
        if (endpoints_[endpoint_id].status.load(std::memory_order_acquire) != 1) return false;

        auto now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        auto last_ns = endpoints_[endpoint_id].last_heartbeat_ns.load(std::memory_order_acquire);
        return (now_ns - last_ns) < header_->timeout_ns;
    }

    // Get all dead endpoints
    [[nodiscard]] std::vector<uint32_t> get_dead_endpoints() const {
        std::vector<uint32_t> dead;
        auto now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        uint32_t count = header_->endpoint_count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count && i < MAX_ENDPOINTS; ++i) {
            if (endpoints_[i].status.load(std::memory_order_acquire) == 1) {
                auto last_ns = endpoints_[i].last_heartbeat_ns.load(std::memory_order_acquire);
                if ((now_ns - last_ns) >= header_->timeout_ns) {
                    dead.push_back(i);
                }
            }
        }
        return dead;
    }

    // Mark endpoint as dead (for cleanup)
    void mark_dead(uint32_t endpoint_id) {
        if (endpoint_id < MAX_ENDPOINTS) {
            endpoints_[endpoint_id].status.store(2, std::memory_order_release);
        }
    }

    // Unregister (on graceful shutdown)
    void unregister() {
        if (my_endpoint_id_ != UINT32_MAX) {
            endpoints_[my_endpoint_id_].status.store(0, std::memory_order_release);
            my_endpoint_id_ = UINT32_MAX;
        }
    }

    // Async heartbeat loop
    asyncio::task::Task<void> run_heartbeat_loop() {
        running_ = true;
        while (running_) {
            heartbeat();
            co_await asyncio::sleep(std::chrono::milliseconds{header_->heartbeat_interval_ns / 1'000'000});
        }
    }

    void stop() { running_ = false; }

    [[nodiscard]] uint64_t heartbeat_interval_ms() const {
        return header_->heartbeat_interval_ns / 1'000'000;
    }

    ~HeartbeatMonitor() {
        unregister();
        if (ptr_) {
            platform::shm_close(ptr_, size_);
            if (owner_) platform::shm_unlink(name_);
        }
    }

    HeartbeatMonitor(HeartbeatMonitor&& o) noexcept
        : name_(std::move(o.name_)), ptr_(o.ptr_), size_(o.size_),
          owner_(o.owner_), header_(o.header_), endpoints_(o.endpoints_),
          my_endpoint_id_(o.my_endpoint_id_) {
        o.ptr_ = nullptr;
        o.my_endpoint_id_ = UINT32_MAX;
    }

    HeartbeatMonitor(const HeartbeatMonitor&) = delete;
    HeartbeatMonitor& operator=(const HeartbeatMonitor&) = delete;

private:
    HeartbeatMonitor(std::string name, void* ptr, size_t size, bool owner,
                     Header* header, EndpointInfo* endpoints)
        : name_(std::move(name)), ptr_(ptr), size_(size), owner_(owner),
          header_(header), endpoints_(endpoints) {}
};

// =============================================================================
// Service Registry - Simple Service Discovery
// =============================================================================

class ServiceRegistry {
public:
    static constexpr uint64_t MAGIC_SR = 0xA510535200002ULL;

    enum class ServiceType : uint32_t {
        SPSC_QUEUE = 1,
        SPMC_QUEUE = 2,
        MPSC_QUEUE = 3,
        RPC_CHANNEL = 4,
        EVENT = 5,
        MEMORY_POOL = 6
    };

    struct ServiceInfoInternal {
        std::atomic<uint32_t> active{0};  // 0=inactive, 1=active
        uint32_t type;
        uint32_t owner_pid;
        uint64_t created_ns;
        uint64_t capacity;
        char name[64];
        char shm_name[64];
    };

    // Copyable version for lookup results
    struct ServiceInfo {
        uint32_t active;
        uint32_t type;
        uint32_t owner_pid;
        uint64_t created_ns;
        uint64_t capacity;
        char name[64];
        char shm_name[64];

        ServiceInfo() = default;
        ServiceInfo(const ServiceInfoInternal& s)
            : active(s.active.load(std::memory_order_acquire)),
              type(s.type), owner_pid(s.owner_pid),
              created_ns(s.created_ns), capacity(s.capacity) {
            std::memcpy(name, s.name, sizeof(name));
            std::memcpy(shm_name, s.shm_name, sizeof(shm_name));
        }
    };

    struct alignas(CACHE_LINE_SIZE) Header {
        uint64_t magic;
        std::atomic<uint32_t> service_count{0};
        std::atomic<uint32_t> version{0};
        char _pad[64 - 24];
    };

    static constexpr size_t MAX_SERVICES = 256;

private:
    std::string name_;
    void* ptr_{nullptr};
    size_t size_{0};
    bool owner_{false};
    Header* header_{nullptr};
    ServiceInfoInternal* services_{nullptr};

public:
    static ServiceRegistry create(const std::string& registry_name = "asyncio-shm-registry") {
        size_t size = sizeof(Header) + sizeof(ServiceInfoInternal) * MAX_SERVICES;
        void* ptr = platform::shm_create(registry_name, size);
        if (!ptr) throw std::runtime_error("Failed to create service registry");

        auto* header = static_cast<Header*>(ptr);
        header->magic = MAGIC_SR;
        header->service_count.store(0, std::memory_order_release);
        header->version.store(0, std::memory_order_release);

        auto* services = reinterpret_cast<ServiceInfoInternal*>(
            static_cast<char*>(ptr) + sizeof(Header));
        for (size_t i = 0; i < MAX_SERVICES; ++i) {
            services[i].active.store(0, std::memory_order_release);
        }

        return ServiceRegistry(registry_name, ptr, size, true, header, services);
    }

    static ServiceRegistry open(const std::string& registry_name = "asyncio-shm-registry") {
        size_t size = 0;
        void* ptr = platform::shm_open_existing(registry_name, size);
        if (!ptr) throw std::runtime_error("Failed to open service registry");

        auto* header = static_cast<Header*>(ptr);
        if (header->magic != MAGIC_SR) {
            platform::shm_close(ptr, size);
            throw std::runtime_error("Invalid service registry magic");
        }

        auto* services = reinterpret_cast<ServiceInfoInternal*>(
            static_cast<char*>(ptr) + sizeof(Header));
        return ServiceRegistry(registry_name, ptr, size, false, header, services);
    }

    // Register a service
    bool register_service(const std::string& service_name, const std::string& shm_name,
                          ServiceType type, uint64_t capacity = 0) {
        // Find empty slot
        for (size_t i = 0; i < MAX_SERVICES; ++i) {
            uint32_t expected = 0;
            if (services_[i].active.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                services_[i].type = static_cast<uint32_t>(type);
                services_[i].owner_pid = static_cast<uint32_t>(getpid());
                services_[i].created_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                services_[i].capacity = capacity;
                std::strncpy(services_[i].name, service_name.c_str(), sizeof(services_[i].name) - 1);
                std::strncpy(services_[i].shm_name, shm_name.c_str(), sizeof(services_[i].shm_name) - 1);

                header_->service_count.fetch_add(1, std::memory_order_relaxed);
                header_->version.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    // Unregister a service
    bool unregister_service(const std::string& service_name) {
        for (size_t i = 0; i < MAX_SERVICES; ++i) {
            if (services_[i].active.load(std::memory_order_acquire) == 1 &&
                std::strncmp(services_[i].name, service_name.c_str(), sizeof(services_[i].name)) == 0) {
                services_[i].active.store(0, std::memory_order_release);
                header_->service_count.fetch_sub(1, std::memory_order_relaxed);
                header_->version.fetch_add(1, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    // Lookup service by name
    [[nodiscard]] std::optional<ServiceInfo> lookup(const std::string& service_name) const {
        for (size_t i = 0; i < MAX_SERVICES; ++i) {
            if (services_[i].active.load(std::memory_order_acquire) == 1 &&
                std::strncmp(services_[i].name, service_name.c_str(), sizeof(services_[i].name)) == 0) {
                ServiceInfo info = services_[i];
                return info;
            }
        }
        return std::nullopt;
    }

    // List all services of a type
    [[nodiscard]] std::vector<ServiceInfo> list_services(std::optional<ServiceType> type = std::nullopt) const {
        std::vector<ServiceInfo> result;
        for (size_t i = 0; i < MAX_SERVICES; ++i) {
            if (services_[i].active.load(std::memory_order_acquire) == 1) {
                if (!type || services_[i].type == static_cast<uint32_t>(*type)) {
                    result.push_back(services_[i]);
                }
            }
        }
        return result;
    }

    // Wait for a service to appear
    asyncio::task::Task<ServiceInfo> wait_for_service(const std::string& service_name,
                                                       std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto info = lookup(service_name)) {
                co_return *info;
            }
            co_await asyncio::sleep(std::chrono::milliseconds{10});
        }
        throw std::runtime_error("Service not found: " + service_name);
    }

    [[nodiscard]] uint32_t version() const {
        return header_->version.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t service_count() const {
        return header_->service_count.load(std::memory_order_acquire);
    }

    ~ServiceRegistry() {
        if (ptr_) {
            platform::shm_close(ptr_, size_);
            if (owner_) platform::shm_unlink(name_);
        }
    }

    ServiceRegistry(ServiceRegistry&& o) noexcept
        : name_(std::move(o.name_)), ptr_(o.ptr_), size_(o.size_),
          owner_(o.owner_), header_(o.header_), services_(o.services_) {
        o.ptr_ = nullptr;
    }

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

private:
    ServiceRegistry(std::string name, void* ptr, size_t size, bool owner,
                    Header* header, ServiceInfoInternal* services)
        : name_(std::move(name)), ptr_(ptr), size_(size), owner_(owner),
          header_(header), services_(services) {}
};

}  // namespace asyncio::net::lowlatency::shm
