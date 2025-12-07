// Asyncio KCP Transport - Reliable UDP with ARQ Protocol
// Integrates KCP (https://github.com/skywind3000/kcp) with asyncio for
// reliable UDP unicast and multicast communication.
//
// Features:
// - Reliable UDP with automatic retransmission (ARQ)
// - 30-40% lower average RTT vs TCP
// - Async/await API using asyncio::task::Task
// - Unicast and multicast support
// - Configurable modes: Normal, Fast, Turbo
//
// Build: Requires kcp/ikcp.c to be compiled and linked

#pragma once

#include <asyncio/task.h>
#include <asyncio/event_loop.h>
#include <asyncio/time.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>
#include <optional>

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
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
    using socket_t = int;
    #define SOCKET_INVALID (-1)
    #define SOCKET_ERROR_VAL (-1)
    inline int closesocket(int fd) { return close(fd); }
#endif

// KCP C header
extern "C" {
#include "kcp/ikcp.h"
}

namespace asyncio::net::lowlatency::kcp {

// =============================================================================
// Configuration
// =============================================================================

inline constexpr size_t MAX_UDP_PACKET = 1472;  // MTU - IP header - UDP header
inline constexpr size_t DEFAULT_MTU = 1400;
inline constexpr uint32_t DEFAULT_CONV = 0x12345678;

enum class Mode {
    NORMAL,     // Default: interval=40ms, nodelay=0
    FAST,       // Fast: interval=30ms, nodelay=0, resend=2
    TURBO,      // Turbo: interval=10ms, nodelay=1, resend=2, nc=1
    FASTEST,    // Fastest (official): interval=20ms, nodelay=1, resend=2, nc=1, rx_minrto=10
    RAW_UDP     // Raw UDP (no KCP, no reliability) - for localhost benchmark comparison
};

struct Config {
    Mode mode = Mode::TURBO;
    uint32_t conv = DEFAULT_CONV;       // Conversation ID
    uint32_t mtu = DEFAULT_MTU;
    uint32_t snd_wnd = 1024;            // Send window (larger for throughput)
    uint32_t rcv_wnd = 1024;            // Receive window (larger for throughput)
    uint32_t interval_ms = 1;           // Update interval (ms) - aggressive for benchmarks
    bool stream_mode = false;           // Stream or message mode
};

// =============================================================================
// Endpoint Address
// =============================================================================

struct Endpoint {
    std::string host;
    uint16_t port;

    Endpoint() = default;
    Endpoint(std::string h, uint16_t p) : host(std::move(h)), port(p) {}

    sockaddr_in to_sockaddr() const {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
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
// UDP Socket Wrapper
// =============================================================================

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { close(); }

    UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = SOCKET_INVALID; }
    UdpSocket& operator=(UdpSocket&& o) noexcept {
        if (this != &o) { close(); fd_ = o.fd_; o.fd_ = SOCKET_INVALID; }
        return *this;
    }

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    static UdpSocket create() {
        UdpSocket sock;
        sock.fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock.fd_ == SOCKET_INVALID) {
            throw std::runtime_error("Failed to create UDP socket");
        }
        sock.set_nonblocking();
        sock.set_reuse_addr();
        return sock;
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

    // Join multicast group
    void join_multicast(const std::string& group_addr, const std::string& local_iface = "0.0.0.0") {
        ip_mreq mreq{};
        inet_pton(AF_INET, group_addr.c_str(), &mreq.imr_multiaddr);
        inet_pton(AF_INET, local_iface.c_str(), &mreq.imr_interface);

        if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&mreq), sizeof(mreq)) < 0) {
            throw std::runtime_error("Failed to join multicast group: " + group_addr);
        }
    }

    // Leave multicast group
    void leave_multicast(const std::string& group_addr, const std::string& local_iface = "0.0.0.0") {
        ip_mreq mreq{};
        inet_pton(AF_INET, group_addr.c_str(), &mreq.imr_multiaddr);
        inet_pton(AF_INET, local_iface.c_str(), &mreq.imr_interface);
        setsockopt(fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   reinterpret_cast<const char*>(&mreq), sizeof(mreq));
    }

    // Set multicast TTL
    void set_multicast_ttl(int ttl) {
        unsigned char ttl_val = static_cast<unsigned char>(ttl);
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<const char*>(&ttl_val), sizeof(ttl_val));
    }

    // Set multicast loopback
    void set_multicast_loop(bool enable) {
        unsigned char loop = enable ? 1 : 0;
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP,
                   reinterpret_cast<const char*>(&loop), sizeof(loop));
    }

    ssize_t send_to(const void* data, size_t len, const Endpoint& dest) {
        auto addr = dest.to_sockaddr();
        return sendto(fd_, static_cast<const char*>(data), len, 0,
                      reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }

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

    void close() {
        if (fd_ != SOCKET_INVALID) {
            closesocket(fd_);
            fd_ = SOCKET_INVALID;
        }
    }

    [[nodiscard]] socket_t fd() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ != SOCKET_INVALID; }

private:
    socket_t fd_ = SOCKET_INVALID;

    void set_nonblocking() {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(fd_, FIONBIO, &mode);
#else
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    void set_reuse_addr() {
        int opt = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif
    }
};

// =============================================================================
// KCP Session - Single reliable connection
// =============================================================================

class KcpSession {
public:
    using OutputCallback = std::function<void(const char*, size_t, const Endpoint&)>;

    KcpSession(uint32_t conv, const Endpoint& remote, OutputCallback output_cb)
        : remote_(remote), output_cb_(std::move(output_cb)) {
        kcp_ = ikcp_create(conv, this);
        if (!kcp_) throw std::runtime_error("Failed to create KCP session");

        ikcp_setoutput(kcp_, &KcpSession::kcp_output);
        configure(Mode::TURBO);
    }

    ~KcpSession() {
        if (kcp_) ikcp_release(kcp_);
    }

    KcpSession(KcpSession&& o) noexcept
        : kcp_(o.kcp_), remote_(std::move(o.remote_)), output_cb_(std::move(o.output_cb_)) {
        o.kcp_ = nullptr;
        if (kcp_) kcp_->user = this;
    }

    KcpSession(const KcpSession&) = delete;
    KcpSession& operator=(const KcpSession&) = delete;

    void configure(Mode mode) {
        switch (mode) {
            case Mode::NORMAL:
                // Normal mode: conservative, TCP-like behavior
                ikcp_nodelay(kcp_, 0, 40, 0, 0);
                break;
            case Mode::FAST:
                // Fast mode: faster retransmit
                ikcp_nodelay(kcp_, 0, 30, 2, 0);
                break;
            case Mode::TURBO:
                // Turbo mode: aggressive for benchmarks
                // nodelay=1, interval=10ms, resend=2, nc=1 (no congestion control)
                ikcp_nodelay(kcp_, 1, 10, 2, 1);
                kcp_->rx_minrto = 10;  // Minimum RTO 10ms
                break;
            case Mode::FASTEST:
                // Fastest mode (official KCP recommendation)
                // From ikcp.h line 393: "// fastest: ikcp_nodelay(kcp, 1, 20, 2, 1)"
                ikcp_nodelay(kcp_, 1, 20, 2, 1);
                kcp_->rx_minrto = 10;  // Minimum RTO 10ms (from README)
                break;
            case Mode::RAW_UDP:
                // Raw UDP mode - same as fastest but will bypass KCP in send
                ikcp_nodelay(kcp_, 1, 20, 2, 1);
                kcp_->rx_minrto = 10;
                break;
        }
        ikcp_wndsize(kcp_, 1024, 1024);  // Large window for throughput
        ikcp_setmtu(kcp_, DEFAULT_MTU);
    }

    void configure(const Config& cfg) {
        configure(cfg.mode);
        ikcp_wndsize(kcp_, static_cast<int>(cfg.snd_wnd), static_cast<int>(cfg.rcv_wnd));
        ikcp_setmtu(kcp_, static_cast<int>(cfg.mtu));
        kcp_->stream = cfg.stream_mode ? 1 : 0;
    }

    // Send data (queued for reliable delivery)
    int send(const void* data, size_t len) {
        std::lock_guard lock(mutex_);
        return ikcp_send(kcp_, static_cast<const char*>(data), static_cast<int>(len));
    }

    // Receive data (returns bytes received, or -1 if no data)
    int recv(void* data, size_t len) {
        std::lock_guard lock(mutex_);
        return ikcp_recv(kcp_, static_cast<char*>(data), static_cast<int>(len));
    }

    // Peek at available data size
    [[nodiscard]] int peek_size() const {
        std::lock_guard lock(mutex_);
        return ikcp_peeksize(const_cast<ikcpcb*>(kcp_));
    }

    // Feed incoming UDP packet to KCP
    int input(const void* data, size_t len) {
        std::lock_guard lock(mutex_);
        return ikcp_input(kcp_, static_cast<const char*>(data), static_cast<long>(len));
    }

    // Update KCP state (call periodically)
    void update(uint32_t current_ms) {
        std::lock_guard lock(mutex_);
        ikcp_update(kcp_, current_ms);
    }

    // Get next update time
    [[nodiscard]] uint32_t check(uint32_t current_ms) const {
        std::lock_guard lock(mutex_);
        return ikcp_check(const_cast<ikcpcb*>(kcp_), current_ms);
    }

    // Flush pending data
    void flush() {
        std::lock_guard lock(mutex_);
        ikcp_flush(kcp_);
    }

    // Get waiting send count
    [[nodiscard]] int wait_snd() const {
        std::lock_guard lock(mutex_);
        return ikcp_waitsnd(const_cast<ikcpcb*>(kcp_));
    }

    [[nodiscard]] const Endpoint& remote() const { return remote_; }
    [[nodiscard]] uint32_t conv() const { return kcp_->conv; }

private:
    ikcpcb* kcp_;
    Endpoint remote_;
    OutputCallback output_cb_;
    mutable std::mutex mutex_;

    static int kcp_output(const char* buf, int len, ikcpcb* kcp, void* user) {
        auto* session = static_cast<KcpSession*>(user);
        if (session->output_cb_) {
            session->output_cb_(buf, static_cast<size_t>(len), session->remote_);
        }
        return 0;
    }
};

// =============================================================================
// KCP Transport - Manages multiple sessions over single UDP socket
// =============================================================================

class KcpTransport {
public:
    using ReceiveCallback = std::function<void(const void*, size_t, const Endpoint&)>;

    explicit KcpTransport(const Config& config = {})
        : config_(config), socket_(UdpSocket::create()) {}

    ~KcpTransport() { stop(); }

    KcpTransport(const KcpTransport&) = delete;
    KcpTransport& operator=(const KcpTransport&) = delete;

    // Bind to local port
    void bind(uint16_t port) {
        socket_.bind(port);
    }

    // Connect to remote endpoint (creates session)
    KcpSession& connect(const Endpoint& remote) {
        auto output_cb = [this](const char* data, size_t len, const Endpoint& dest) {
            socket_.send_to(data, len, dest);
        };

        std::lock_guard lock(sessions_mutex_);
        auto [it, inserted] = sessions_.try_emplace(
            remote, config_.conv, remote, output_cb);
        if (inserted) {
            it->second.configure(config_);
        }
        return it->second;
    }

    // Join multicast group
    void join_multicast(const std::string& group, uint16_t port) {
        socket_.bind(port);
        socket_.join_multicast(group);
        socket_.set_multicast_loop(false);
        multicast_group_ = Endpoint{group, port};
    }

    // Send to multicast group (unreliable, use for discovery/heartbeat)
    ssize_t send_multicast(const void* data, size_t len) {
        if (multicast_group_.port == 0) return -1;
        return socket_.send_to(data, len, multicast_group_);
    }

    // Send reliable data to endpoint
    int send(const Endpoint& dest, const void* data, size_t len) {
        std::lock_guard lock(sessions_mutex_);
        auto it = sessions_.find(dest);
        if (it == sessions_.end()) {
            // Auto-create session
            auto output_cb = [this](const char* d, size_t l, const Endpoint& ep) {
                socket_.send_to(d, l, ep);
            };
            auto [new_it, _] = sessions_.try_emplace(dest, config_.conv, dest, output_cb);
            new_it->second.configure(config_);
            it = new_it;
        }
        int result = it->second.send(data, len);
        // Immediate flush for lower latency
        it->second.flush();
        return result;
    }

    // Set receive callback
    void on_receive(ReceiveCallback cb) {
        receive_cb_ = std::move(cb);
    }

    // Start the transport (runs update loop in background)
    void start() {
        if (running_.exchange(true)) return;

        update_thread_ = std::thread([this] {
            std::vector<char> recv_buf(MAX_UDP_PACKET);

            while (running_) {
                auto now_ms = current_time_ms();

                // Update all sessions
                {
                    std::lock_guard lock(sessions_mutex_);
                    for (auto& [ep, session] : sessions_) {
                        session.update(now_ms);

                        // Check for received data
                        while (session.peek_size() > 0) {
                            int n = session.recv(recv_buf.data(), recv_buf.size());
                            if (n > 0 && receive_cb_) {
                                receive_cb_(recv_buf.data(), static_cast<size_t>(n), ep);
                            }
                        }
                    }
                }

                // Poll for incoming UDP packets (process multiple at once)
                int poll_count = 0;
                while (socket_.poll_read(0) && poll_count++ < 100) {
                    Endpoint sender;
                    ssize_t n = socket_.recv_from(recv_buf.data(), recv_buf.size(), sender);
                    if (n > 0) {
                        // Check if it's a KCP packet (has valid conv)
                        if (n >= 4) {
                            uint32_t conv = ikcp_getconv(recv_buf.data());
                            if (conv == config_.conv) {
                                // Feed to appropriate session
                                std::lock_guard lock(sessions_mutex_);
                                auto it = sessions_.find(sender);
                                if (it != sessions_.end()) {
                                    it->second.input(recv_buf.data(), static_cast<size_t>(n));
                                } else {
                                    // Auto-create session for incoming connection
                                    auto output_cb = [this](const char* d, size_t l, const Endpoint& ep) {
                                        socket_.send_to(d, l, ep);
                                    };
                                    auto [new_it, _] = sessions_.try_emplace(
                                        sender, config_.conv, sender, output_cb);
                                    new_it->second.configure(config_);
                                    new_it->second.input(recv_buf.data(), static_cast<size_t>(n));
                                }
                            } else if (receive_cb_) {
                                // Not a KCP packet, deliver raw (multicast etc.)
                                receive_cb_(recv_buf.data(), static_cast<size_t>(n), sender);
                            }
                        }
                    }
                }

                // Sleep based on interval - KCP recommends 10-100ms update interval
                // For latency-sensitive apps, use shorter intervals but never below 1ms
                if (config_.interval_ms == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds{500});
                } else if (config_.interval_ms <= 5) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                } else {
                    // Sleep for half the interval to allow for processing time
                    std::this_thread::sleep_for(std::chrono::milliseconds{config_.interval_ms / 2});
                }
            }
        });
    }

    // Stop the transport
    void stop() {
        running_ = false;
        if (update_thread_.joinable()) {
            update_thread_.join();
        }
    }

    // Async send with coroutine
    asyncio::task::Task<int> async_send(const Endpoint& dest, const void* data, size_t len) {
        int result = send(dest, data, len);
        co_return result;
    }

    // Async receive (waits for data with timeout)
    asyncio::task::Task<std::optional<std::pair<std::vector<char>, Endpoint>>>
    async_recv(std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
        auto deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            std::lock_guard lock(sessions_mutex_);
            for (auto& [ep, session] : sessions_) {
                if (session.peek_size() > 0) {
                    std::vector<char> buf(session.peek_size());
                    int n = session.recv(buf.data(), buf.size());
                    if (n > 0) {
                        buf.resize(static_cast<size_t>(n));
                        co_return std::make_pair(std::move(buf), ep);
                    }
                }
            }
            co_await asyncio::sleep(std::chrono::milliseconds{1});
        }
        co_return std::nullopt;
    }

    [[nodiscard]] size_t session_count() const {
        std::lock_guard lock(sessions_mutex_);
        return sessions_.size();
    }

    [[nodiscard]] bool running() const { return running_; }

private:
    Config config_;
    UdpSocket socket_;
    Endpoint multicast_group_;
    std::unordered_map<Endpoint, KcpSession, EndpointHash> sessions_;
    mutable std::mutex sessions_mutex_;
    std::atomic<bool> running_{false};
    std::thread update_thread_;
    ReceiveCallback receive_cb_;

    static uint32_t current_time_ms() {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
};

// =============================================================================
// KCP Multicast Group - Reliable multicast using KCP
// =============================================================================

class KcpMulticastGroup {
public:
    using MessageCallback = std::function<void(const void*, size_t, const Endpoint&)>;

    KcpMulticastGroup(const std::string& group_addr, uint16_t port, const Config& config = {})
        : group_(group_addr, port), transport_(config) {
        transport_.join_multicast(group_addr, port);
    }

    // Set message handler
    void on_message(MessageCallback cb) {
        message_cb_ = std::move(cb);
        transport_.on_receive([this](const void* data, size_t len, const Endpoint& sender) {
            if (message_cb_) {
                message_cb_(data, len, sender);
            }
        });
    }

    // Start receiving
    void start() {
        transport_.start();
    }

    // Stop receiving
    void stop() {
        transport_.stop();
    }

    // Send to all group members (unreliable multicast)
    ssize_t broadcast(const void* data, size_t len) {
        return transport_.send_multicast(data, len);
    }

    // Send reliable to specific member
    int send_reliable(const Endpoint& dest, const void* data, size_t len) {
        return transport_.send(dest, data, len);
    }

    // Get group address
    [[nodiscard]] const Endpoint& group() const { return group_; }

private:
    Endpoint group_;
    KcpTransport transport_;
    MessageCallback message_cb_;
};

// =============================================================================
// Helper: Create configured transport
// =============================================================================

inline KcpTransport create_transport(Mode mode = Mode::TURBO, uint32_t conv = DEFAULT_CONV) {
    Config config;
    config.mode = mode;
    config.conv = conv;
    return KcpTransport(config);
}

inline KcpTransport create_turbo_transport(uint32_t conv = DEFAULT_CONV) {
    return create_transport(Mode::TURBO, conv);
}

}  // namespace asyncio::net::lowlatency::kcp
