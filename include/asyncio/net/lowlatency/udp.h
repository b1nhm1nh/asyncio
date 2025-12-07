// Asyncio Low-Latency UDP Transport - Raw UDP without ARQ
// For use cases where latency is more important than reliability:
// - Localhost benchmarks
// - LAN environments with minimal packet loss
// - Market data streaming (can tolerate occasional drops)
//
// Features:
// - Minimal overhead (~1-2μs vs KCP's ~2ms)
// - Async/await API using asyncio::task::Task
// - Zero-copy receive path option
// - Batching support for throughput
// - Optional SO_BUSY_POLL for Linux (saves ~7μs)
// - Multicast support

#pragma once

#include <asyncio/task.h>
#include <asyncio/event_loop.h>
#include <asyncio/time.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

// CPU intrinsics for spin-wait optimization
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
    #define CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
    #define CPU_PAUSE() ((void)0)
#endif

// Platform-specific socket includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socket_t = SOCKET;
    #define SOCKET_INVALID INVALID_SOCKET
    #define SOCKET_ERROR_VAL SOCKET_ERROR
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/udp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <errno.h>
    using socket_t = int;
    #define SOCKET_INVALID (-1)
    #define SOCKET_ERROR_VAL (-1)
    inline int closesocket(int fd) { return close(fd); }
#endif

namespace asyncio::net::lowlatency {

// =============================================================================
// Configuration
// =============================================================================

inline constexpr size_t MAX_UDP_PACKET = 65507;    // Max UDP payload (65535 - 20 IP - 8 UDP)
inline constexpr size_t OPTIMAL_UDP_PACKET = 1472; // MTU (1500) - IP header (20) - UDP header (8)
inline constexpr size_t DEFAULT_RECV_BUFFER = 2 * 1024 * 1024;  // 2MB receive buffer
inline constexpr size_t DEFAULT_SEND_BUFFER = 2 * 1024 * 1024;  // 2MB send buffer

struct Config {
    size_t recv_buffer_size = DEFAULT_RECV_BUFFER;
    size_t send_buffer_size = DEFAULT_SEND_BUFFER;
    bool reuse_addr = true;
    bool reuse_port = true;
    bool nonblocking = true;
    int busy_poll_us = 0;        // SO_BUSY_POLL value (Linux only, 0 = disabled)
    int priority = 0;            // SO_PRIORITY (Linux only, 0 = default)
    bool disable_checksum = false;  // UDP_NOCHECKSUM (not recommended)

    // Additional low-latency options
    bool low_delay_tos = true;   // IP_TOS with IPTOS_LOWDELAY (minimize delay)
    bool zerocopy_send = false;  // MSG_ZEROCOPY (Linux 4.14+, requires kernel support)
    int incoming_cpu = -1;       // SO_INCOMING_CPU (Linux, -1 = disabled)
    bool hw_timestamps = false;  // SO_TIMESTAMPNS for hardware timestamps
    int rcv_lowat = 1;           // SO_RCVLOWAT - minimum bytes before waking (1 = immediate)
};

// =============================================================================
// Endpoint Address
// =============================================================================

struct Endpoint {
    std::string host;
    uint16_t port{0};

    Endpoint() = default;
    Endpoint(std::string h, uint16_t p) : host(std::move(h)), port(p) {}

    sockaddr_in to_sockaddr() const {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (host.empty() || host == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        }
        return addr;
    }

    static Endpoint from_sockaddr(const sockaddr_in& addr) {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        return Endpoint{buf, ntohs(addr.sin_port)};
    }

    bool operator==(const Endpoint& o) const {
        return host == o.host && port == o.port;
    }

    std::string to_string() const {
        return host + ":" + std::to_string(port);
    }
};

struct EndpointHash {
    size_t operator()(const Endpoint& e) const {
        return std::hash<std::string>{}(e.host) ^ (std::hash<uint16_t>{}(e.port) << 16);
    }
};

// =============================================================================
// Low-Latency UDP Socket
// =============================================================================

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { close(); }

    UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_), connected_(o.connected_) {
        o.fd_ = SOCKET_INVALID;
        o.connected_ = false;
    }

    UdpSocket& operator=(UdpSocket&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = o.fd_;
            connected_ = o.connected_;
            o.fd_ = SOCKET_INVALID;
            o.connected_ = false;
        }
        return *this;
    }

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Create and configure socket with low-latency settings
    static UdpSocket create(const Config& config = {}) {
        UdpSocket sock;
        sock.fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock.fd_ == SOCKET_INVALID) {
            throw std::runtime_error("Failed to create UDP socket");
        }
        sock.configure(config);
        return sock;
    }

    void configure(const Config& config) {
        if (config.nonblocking) {
            set_nonblocking();
        }
        if (config.reuse_addr) {
            set_option(SOL_SOCKET, SO_REUSEADDR, 1);
        }
#ifdef SO_REUSEPORT
        if (config.reuse_port) {
            set_option(SOL_SOCKET, SO_REUSEPORT, 1);
        }
#endif
        if (config.recv_buffer_size > 0) {
            set_option(SOL_SOCKET, SO_RCVBUF, static_cast<int>(config.recv_buffer_size));
        }
        if (config.send_buffer_size > 0) {
            set_option(SOL_SOCKET, SO_SNDBUF, static_cast<int>(config.send_buffer_size));
        }

        // IP_TOS for low-delay QoS marking (works on all platforms)
        if (config.low_delay_tos) {
#ifdef IPTOS_LOWDELAY
            int tos = IPTOS_LOWDELAY;  // 0x10 - minimize delay
            setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
#endif
        }

        // Minimum receive watermark - wake immediately on any data
        if (config.rcv_lowat > 0) {
            set_option(SOL_SOCKET, SO_RCVLOWAT, config.rcv_lowat);
        }

#ifdef __linux__
        // Linux-specific low-latency options
        if (config.busy_poll_us > 0) {
            set_option(SOL_SOCKET, SO_BUSY_POLL, config.busy_poll_us);
        }
        if (config.priority > 0) {
            set_option(SOL_SOCKET, SO_PRIORITY, config.priority);
        }
        // SO_INCOMING_CPU for CPU affinity (Linux 3.19+)
        if (config.incoming_cpu >= 0) {
#ifdef SO_INCOMING_CPU
            set_option(SOL_SOCKET, SO_INCOMING_CPU, config.incoming_cpu);
#endif
        }
        // Hardware timestamps for accurate latency measurement
        if (config.hw_timestamps) {
#ifdef SO_TIMESTAMPNS
            int ts = 1;
            setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPNS, &ts, sizeof(ts));
#endif
        }
        // MSG_ZEROCOPY requires SO_ZEROCOPY on socket (Linux 4.14+)
        if (config.zerocopy_send) {
#ifdef SO_ZEROCOPY
            int zc = 1;
            setsockopt(fd_, SOL_SOCKET, SO_ZEROCOPY, &zc, sizeof(zc));
            zerocopy_enabled_ = true;
#endif
        }
#endif

        // Note: UDP_CORK could be used for batching on Linux
    }

    void bind(const Endpoint& endpoint) {
        auto addr = endpoint.to_sockaddr();
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR_VAL) {
            throw std::runtime_error("Failed to bind UDP socket to " + endpoint.to_string());
        }
    }

    void bind(uint16_t port) {
        bind(Endpoint{"0.0.0.0", port});
    }

    // Connect for connected UDP (eliminates per-send destination lookup)
    void connect(const Endpoint& endpoint) {
        auto addr = endpoint.to_sockaddr();
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR_VAL) {
            throw std::runtime_error("Failed to connect UDP socket to " + endpoint.to_string());
        }
        connected_ = true;
        remote_ = endpoint;
    }

    // -------------------------------------------------------------------------
    // Multicast
    // -------------------------------------------------------------------------

    void join_multicast(const std::string& group_addr, const std::string& local_iface = "0.0.0.0") {
        ip_mreq mreq{};
        inet_pton(AF_INET, group_addr.c_str(), &mreq.imr_multiaddr);
        inet_pton(AF_INET, local_iface.c_str(), &mreq.imr_interface);

        if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&mreq), sizeof(mreq)) < 0) {
            throw std::runtime_error("Failed to join multicast group: " + group_addr);
        }
    }

    void leave_multicast(const std::string& group_addr, const std::string& local_iface = "0.0.0.0") {
        ip_mreq mreq{};
        inet_pton(AF_INET, group_addr.c_str(), &mreq.imr_multiaddr);
        inet_pton(AF_INET, local_iface.c_str(), &mreq.imr_interface);
        setsockopt(fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   reinterpret_cast<const char*>(&mreq), sizeof(mreq));
    }

    void set_multicast_ttl(int ttl) {
        unsigned char ttl_val = static_cast<unsigned char>(ttl);
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<const char*>(&ttl_val), sizeof(ttl_val));
    }

    void set_multicast_loop(bool enable) {
        unsigned char loop = enable ? 1 : 0;
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP,
                   reinterpret_cast<const char*>(&loop), sizeof(loop));
    }

    // -------------------------------------------------------------------------
    // Send Operations
    // -------------------------------------------------------------------------

    // Send to connected endpoint (fastest path)
    ssize_t send(const void* data, size_t len) {
        return ::send(fd_, static_cast<const char*>(data), len, 0);
    }

    // Send to specific endpoint
    ssize_t send_to(const void* data, size_t len, const Endpoint& dest) {
        auto addr = dest.to_sockaddr();
        return sendto(fd_, static_cast<const char*>(data), len, 0,
                      reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }

    // Send to pre-computed sockaddr (avoids repeated conversion)
    ssize_t send_to_raw(const void* data, size_t len, const sockaddr_in& dest) {
        return sendto(fd_, static_cast<const char*>(data), len, 0,
                      reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
    }

#ifndef _WIN32
    // Batch send using sendmmsg (Linux only) - up to 1024 messages
    int send_batch(const std::vector<std::pair<const void*, size_t>>& messages,
                   const Endpoint& dest) {
#ifdef __linux__
        if (messages.empty()) return 0;

        auto addr = dest.to_sockaddr();
        std::vector<mmsghdr> msgs(messages.size());
        std::vector<iovec> iovs(messages.size());

        for (size_t i = 0; i < messages.size(); ++i) {
            iovs[i].iov_base = const_cast<void*>(messages[i].first);
            iovs[i].iov_len = messages[i].second;
            msgs[i].msg_hdr.msg_iov = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            msgs[i].msg_hdr.msg_name = &addr;
            msgs[i].msg_hdr.msg_namelen = sizeof(addr);
        }

        return sendmmsg(fd_, msgs.data(), static_cast<unsigned int>(msgs.size()), 0);
#else
        // Fallback for non-Linux
        int sent = 0;
        for (const auto& [data, len] : messages) {
            if (send_to(data, len, dest) > 0) ++sent;
        }
        return sent;
#endif
    }
#endif

    // -------------------------------------------------------------------------
    // Receive Operations
    // -------------------------------------------------------------------------

    // Receive from connected endpoint (fastest path)
    ssize_t recv(void* data, size_t len) {
        return ::recv(fd_, static_cast<char*>(data), len, 0);
    }

    // Receive with sender address
    ssize_t recv_from(void* data, size_t len, Endpoint& sender) {
        sockaddr_in addr{};
        socklen_t addr_len = sizeof(addr);
        ssize_t n = recvfrom(fd_, static_cast<char*>(data), len, 0,
                             reinterpret_cast<sockaddr*>(&addr), &addr_len);
        if (n > 0) {
            sender = Endpoint::from_sockaddr(addr);
        }
        return n;
    }

    // Receive with raw sockaddr (avoids Endpoint allocation)
    ssize_t recv_from_raw(void* data, size_t len, sockaddr_in& sender) {
        socklen_t addr_len = sizeof(sender);
        return recvfrom(fd_, static_cast<char*>(data), len, 0,
                        reinterpret_cast<sockaddr*>(&sender), &addr_len);
    }

#ifndef _WIN32
    // Batch receive using recvmmsg (Linux only)
    int recv_batch(std::vector<std::vector<char>>& buffers,
                   std::vector<Endpoint>& senders,
                   size_t max_messages = 64) {
#ifdef __linux__
        buffers.resize(max_messages);
        senders.resize(max_messages);

        std::vector<mmsghdr> msgs(max_messages);
        std::vector<iovec> iovs(max_messages);
        std::vector<sockaddr_in> addrs(max_messages);

        for (size_t i = 0; i < max_messages; ++i) {
            buffers[i].resize(OPTIMAL_UDP_PACKET);
            iovs[i].iov_base = buffers[i].data();
            iovs[i].iov_len = buffers[i].size();
            msgs[i].msg_hdr.msg_iov = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            msgs[i].msg_hdr.msg_name = &addrs[i];
            msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
        }

        int n = recvmmsg(fd_, msgs.data(), static_cast<unsigned int>(max_messages), 0, nullptr);
        if (n > 0) {
            buffers.resize(static_cast<size_t>(n));
            senders.resize(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                buffers[i].resize(msgs[i].msg_len);
                senders[i] = Endpoint::from_sockaddr(addrs[i]);
            }
        }
        return n;
#else
        // Fallback for non-Linux
        buffers.clear();
        senders.clear();
        std::vector<char> buf(OPTIMAL_UDP_PACKET);
        Endpoint sender;
        ssize_t n = recv_from(buf.data(), buf.size(), sender);
        if (n > 0) {
            buf.resize(static_cast<size_t>(n));
            buffers.push_back(std::move(buf));
            senders.push_back(sender);
            return 1;
        }
        return 0;
#endif
    }
#endif

    // -------------------------------------------------------------------------
    // Poll Operations
    // -------------------------------------------------------------------------

    bool poll_read(int timeout_ms) {
#ifdef _WIN32
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd_, &readfds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        return select(0, &readfds, nullptr, nullptr, &tv) > 0;
#else
        pollfd pfd{fd_, POLLIN, 0};
        return poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN);
#endif
    }

    bool poll_write(int timeout_ms) {
#ifdef _WIN32
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(fd_, &writefds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        return select(0, nullptr, &writefds, nullptr, &tv) > 0;
#else
        pollfd pfd{fd_, POLLOUT, 0};
        return poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT);
#endif
    }

    // Non-blocking check if readable
    bool readable() const { return const_cast<UdpSocket*>(this)->poll_read(0); }

    // -------------------------------------------------------------------------
    // Utility
    // -------------------------------------------------------------------------

    void close() {
        if (fd_ != SOCKET_INVALID) {
            closesocket(fd_);
            fd_ = SOCKET_INVALID;
        }
        connected_ = false;
    }

    [[nodiscard]] socket_t fd() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ != SOCKET_INVALID; }
    [[nodiscard]] bool is_connected() const { return connected_; }
    [[nodiscard]] const Endpoint& remote() const { return remote_; }

private:
    socket_t fd_ = SOCKET_INVALID;
    bool connected_ = false;
    bool zerocopy_enabled_ = false;
    Endpoint remote_;

    void set_nonblocking() {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(fd_, FIONBIO, &mode);
#else
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    template<typename T>
    void set_option(int level, int optname, T value) {
        setsockopt(fd_, level, optname, reinterpret_cast<const char*>(&value), sizeof(value));
    }
};

// =============================================================================
// UDP Transport - High-level async transport
// =============================================================================

class UdpTransport {
public:
    using ReceiveCallback = std::function<void(const void*, size_t, const Endpoint&)>;
    using ReceiveCallbackZeroCopy = std::function<void(std::span<const char>, const Endpoint&)>;

    explicit UdpTransport(const Config& config = {})
        : config_(config), socket_(UdpSocket::create(config)) {}

    ~UdpTransport() { stop(); }

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // -------------------------------------------------------------------------
    // Setup
    // -------------------------------------------------------------------------

    void bind(uint16_t port) { socket_.bind(port); }
    void bind(const Endpoint& ep) { socket_.bind(ep); }

    // Connect for single-peer communication (faster send path)
    void connect(const Endpoint& remote) { socket_.connect(remote); }

    // Multicast
    void join_multicast(const std::string& group, uint16_t port) {
        socket_.bind(port);
        socket_.join_multicast(group);
        socket_.set_multicast_loop(false);
        multicast_group_ = Endpoint{group, port};
    }

    void leave_multicast() {
        if (!multicast_group_.host.empty()) {
            socket_.leave_multicast(multicast_group_.host);
            multicast_group_ = {};
        }
    }

    // -------------------------------------------------------------------------
    // Send Operations
    // -------------------------------------------------------------------------

    // Send to connected endpoint (fastest)
    ssize_t send(const void* data, size_t len) {
        return socket_.send(data, len);
    }

    // Send to specific endpoint
    ssize_t send_to(const void* data, size_t len, const Endpoint& dest) {
        return socket_.send_to(data, len, dest);
    }

    // Send to multicast group
    ssize_t send_multicast(const void* data, size_t len) {
        if (multicast_group_.port == 0) return -1;
        return socket_.send_to(data, len, multicast_group_);
    }

    // -------------------------------------------------------------------------
    // Callbacks
    // -------------------------------------------------------------------------

    void on_receive(ReceiveCallback cb) {
        receive_cb_ = std::move(cb);
    }

    void on_receive_zerocopy(ReceiveCallbackZeroCopy cb) {
        receive_cb_zc_ = std::move(cb);
    }

    // -------------------------------------------------------------------------
    // Background Receiver Thread
    // -------------------------------------------------------------------------

    void start() {
        if (running_.exchange(true)) return;

        receiver_thread_ = std::thread([this] {
            std::vector<char> recv_buf(OPTIMAL_UDP_PACKET);

            while (running_) {
                // Spin-poll for lowest latency (at cost of CPU)
                if (!socket_.poll_read(1)) continue;

                Endpoint sender;
                ssize_t n = socket_.recv_from(recv_buf.data(), recv_buf.size(), sender);
                if (n > 0) {
                    if (receive_cb_zc_) {
                        receive_cb_zc_(std::span<const char>(recv_buf.data(), static_cast<size_t>(n)), sender);
                    } else if (receive_cb_) {
                        receive_cb_(recv_buf.data(), static_cast<size_t>(n), sender);
                    }
                }
            }
        });
    }

    void stop() {
        running_ = false;
        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }

    // -------------------------------------------------------------------------
    // Async API (using asyncio coroutines)
    // -------------------------------------------------------------------------

    // Async send
    asyncio::task::Task<ssize_t> async_send(const void* data, size_t len) {
        co_return socket_.send(data, len);
    }

    asyncio::task::Task<ssize_t> async_send_to(const void* data, size_t len, const Endpoint& dest) {
        co_return socket_.send_to(data, len, dest);
    }

    // Async receive with timeout
    asyncio::task::Task<std::optional<std::pair<std::vector<char>, Endpoint>>>
    async_recv(std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;

        std::vector<char> buf(OPTIMAL_UDP_PACKET);
        while (std::chrono::steady_clock::now() < deadline) {
            if (socket_.poll_read(0)) {
                Endpoint sender;
                ssize_t n = socket_.recv_from(buf.data(), buf.size(), sender);
                if (n > 0) {
                    buf.resize(static_cast<size_t>(n));
                    co_return std::make_pair(std::move(buf), sender);
                }
            }
            co_await asyncio::sleep(std::chrono::milliseconds{1});
        }
        co_return std::nullopt;
    }

    // -------------------------------------------------------------------------
    // Info
    // -------------------------------------------------------------------------

    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] const Endpoint& multicast_group() const { return multicast_group_; }
    [[nodiscard]] socket_t fd() const { return socket_.fd(); }

private:
    Config config_;
    UdpSocket socket_;
    Endpoint multicast_group_;
    std::atomic<bool> running_{false};
    std::thread receiver_thread_;
    ReceiveCallback receive_cb_;
    ReceiveCallbackZeroCopy receive_cb_zc_;
};

// =============================================================================
// Synchronous UDP Pair - For lowest latency benchmarks
// Uses busy-polling instead of threads for sub-microsecond latency
// =============================================================================

class UdpPair {
public:
    UdpPair(const Endpoint& local, const Endpoint& remote, const Config& config = {})
        : socket_(UdpSocket::create(config)), remote_(remote) {
        socket_.bind(local);
        remote_addr_ = remote.to_sockaddr();
    }

    // Send with pre-computed destination (fastest path)
    ssize_t send(const void* data, size_t len) {
        return socket_.send_to_raw(data, len, remote_addr_);
    }

    // Blocking receive with timeout
    ssize_t recv(void* data, size_t len, int timeout_ms = 1000) {
        if (!socket_.poll_read(timeout_ms)) return -1;
        return socket_.recv(data, len);
    }

    // Non-blocking receive
    ssize_t try_recv(void* data, size_t len) {
        if (!socket_.poll_read(0)) return 0;
        return socket_.recv(data, len);
    }

    // Busy-poll receive (lowest latency, burns CPU)
    // Uses CPU_PAUSE() to reduce power consumption and improve SMT performance
    ssize_t busy_recv(void* data, size_t len, uint64_t max_spins = 1000000) {
        for (uint64_t i = 0; i < max_spins; ++i) {
            if (socket_.poll_read(0)) {
                return socket_.recv(data, len);
            }
            CPU_PAUSE();  // Hint to CPU that we're spinning
        }
        return -1;
    }

    [[nodiscard]] const Endpoint& remote() const { return remote_; }
    [[nodiscard]] socket_t fd() const { return socket_.fd(); }

private:
    UdpSocket socket_;
    Endpoint remote_;
    sockaddr_in remote_addr_;
};

// =============================================================================
// Factory Functions
// =============================================================================

inline UdpTransport create_transport(const Config& config = {}) {
    return UdpTransport(config);
}

// Low-latency configuration with common optimizations
inline UdpTransport create_low_latency_transport() {
    Config config;
    config.recv_buffer_size = 4 * 1024 * 1024;  // 4MB
    config.send_buffer_size = 4 * 1024 * 1024;  // 4MB
    config.low_delay_tos = true;                 // IPTOS_LOWDELAY
    config.rcv_lowat = 1;                        // Wake on any data
#ifdef __linux__
    config.busy_poll_us = 50;                    // 50μs busy poll
#endif
    return UdpTransport(config);
}

// Ultra-low-latency configuration with all optimizations
// Use for HFT, market data, or benchmarks where every microsecond counts
inline UdpTransport create_ultra_low_latency_transport() {
    Config config;
    config.recv_buffer_size = 8 * 1024 * 1024;  // 8MB - larger to avoid drops
    config.send_buffer_size = 8 * 1024 * 1024;  // 8MB
    config.low_delay_tos = true;                 // IPTOS_LOWDELAY QoS
    config.rcv_lowat = 1;                        // Wake immediately
#ifdef __linux__
    config.busy_poll_us = 100;                   // 100μs busy poll (saves ~7μs)
    config.priority = 6;                         // High priority
    config.hw_timestamps = true;                 // For accurate latency
#endif
    return UdpTransport(config);
}

inline UdpPair create_pair(uint16_t local_port, const Endpoint& remote) {
    Config config;
    config.recv_buffer_size = 4 * 1024 * 1024;
    config.send_buffer_size = 4 * 1024 * 1024;
    return UdpPair(Endpoint{"0.0.0.0", local_port}, remote, config);
}

}  // namespace asyncio::net::lowlatency
