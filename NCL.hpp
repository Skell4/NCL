// NCL 0.3 - Network Communication Library
// C++17, header-only; synchronous IPv4/IPv6 API with deadlines and cancellation.
#ifndef NCL_HPP_INCLUDED
#define NCL_HPP_INCLUDED

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#    define NCL_DETAIL_UNDEF_NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef NCL_DETAIL_UNDEF_NOMINMAX
#    undef NOMINMAX
#    undef NCL_DETAIL_UNDEF_NOMINMAX
#  endif
#  if defined(_MSC_VER)
#    pragma comment(lib, "Ws2_32.lib")
#  endif
#elif defined(__linux__)
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#else
#  error "NCL 0.3 supports Windows and Linux only"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ncl {

inline constexpr std::size_t defaultMaxMessageSize = 1024 * 1024;
// Conservative shared payload cap for IPv4 and IPv6; no IPv6 jumbograms.
inline constexpr std::size_t maxDatagramSize = 65507;

enum class ErrorKind {
    System,
    Resolution,
    InvalidArgument,
    InvalidState,
    MessageTooLarge,
    UnexpectedEof,
    DatagramTruncated,
    Timeout,
    Cancelled
};

class NetworkError : public std::runtime_error {
public:
    NetworkError(ErrorKind kind, const std::string& message, int nativeCode = 0,
                 std::size_t bytesTransferred = 0)
        : std::runtime_error(message), kind_(kind), nativeCode_(nativeCode),
          bytesTransferred_(bytesTransferred) {}

    ErrorKind kind() const noexcept { return kind_; }
    // System: WSA/errno. Resolution: getaddrinfo code. Other kinds: 0.
    int nativeCode() const noexcept { return nativeCode_; }
    // Bytes transferred by this call, including framing bytes for messages.
    std::size_t bytesTransferred() const noexcept { return bytesTransferred_; }

private:
    ErrorKind kind_;
    int nativeCode_;
    std::size_t bytesTransferred_;
};

class CancellationSource;

class CancellationToken {
public:
    CancellationToken() noexcept = default;
    bool canBeCancelled() const noexcept { return static_cast<bool>(state_); }
    bool isCancellationRequested() const noexcept {
        return state_ && state_->load(std::memory_order_acquire);
    }
private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> state) noexcept
        : state_(std::move(state)) {}
    std::shared_ptr<std::atomic<bool>> state_;
};

// Copies share the same, one-way cancellation request. No reset: make a new
// source for a new group of operations. Only requestCancellation() is needed
// from the other thread; the socket itself remains owned by its I/O thread.
class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<std::atomic<bool>>(false)) {}
    CancellationToken token() const noexcept { return CancellationToken(state_); }
    bool requestCancellation() const noexcept {
        return state_ && !state_->exchange(true, std::memory_order_acq_rel);
    }
private:
    std::shared_ptr<std::atomic<bool>> state_;
};

struct WaitOptions {
    // Zero = no deadline; negative durations are rejected.
    std::chrono::milliseconds timeout{0};
    CancellationToken cancellation{};
};

enum class AddressFamily { Unspecified, IPv4, IPv6 };
enum class IPv6Mode { IPv6Only, DualStack };

struct Endpoint {
    std::string address;
    std::uint16_t port = 0;
    AddressFamily family = AddressFamily::Unspecified;
    std::uint32_t scopeId = 0;
};

struct DatagramPacket {
    std::string data;    // Binary-safe; no implicit text encoding conversion.
    std::string address;
    std::uint16_t port = 0;
    AddressFamily family = AddressFamily::Unspecified; // Optional destination hint.
    std::uint32_t scopeId = 0; // IPv6 interface index; address may also have %zone.
};

namespace detail {

// Bound individual native calls so large transfers check deadline/cancellation
// regularly, even if an OS would accept a very large buffer in one call.
inline constexpr std::size_t ioChunkSize = 64 * 1024;

#if defined(_WIN32)
using NativeSocket = SOCKET;
using AddressLength = int;
inline constexpr NativeSocket invalidSocket = INVALID_SOCKET;
inline int lastError() noexcept { return ::WSAGetLastError(); }
inline bool interrupted(int code) noexcept { return code == WSAEINTR; }
inline bool wouldBlock(int code) noexcept { return code == WSAEWOULDBLOCK; }
inline bool connectPending(int code) noexcept { return wouldBlock(code); }
inline void closeNative(NativeSocket socket) noexcept { ::closesocket(socket); }
#else
using NativeSocket = int;
using AddressLength = socklen_t;
inline constexpr NativeSocket invalidSocket = -1;
inline int lastError() noexcept { return errno; }
inline bool interrupted(int code) noexcept { return code == EINTR; }
inline bool wouldBlock(int code) noexcept { return code == EAGAIN || code == EWOULDBLOCK; }
inline bool connectPending(int code) noexcept { return code == EINPROGRESS || code == EINTR; }
// Do not retry close() on Linux: even on EINTR the fd may have been released.
inline void closeNative(NativeSocket socket) noexcept { ::close(socket); }
#endif

[[noreturn]] inline void fail(ErrorKind kind, const std::string& message) {
    throw NetworkError(kind, message);
}

[[noreturn]] inline void systemError(const char* operation, int code) {
    throw NetworkError(ErrorKind::System,
        std::string(operation) + ": " + std::system_category().message(code) +
        " (native code " + std::to_string(code) + ")", code);
}

inline void validateWaitOptions(const WaitOptions& options) {
    if (options.timeout.count() < 0)
        fail(ErrorKind::InvalidArgument, "Timeout must not be negative");
    if (options.cancellation.isCancellationRequested())
        fail(ErrorKind::Cancelled, "Operation cancelled");
}

class WaitContext {
public:
    explicit WaitContext(const WaitOptions& options) : cancellation_(options.cancellation) {
        validateWaitOptions(options);
        if (options.timeout.count() != 0) {
            const auto now = Clock::now();
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::time_point::max() - now);
            deadline_ = options.timeout >= remaining
                ? Clock::time_point::max() : now + options.timeout;
        }
    }
    void check() const {
        if (cancellation_.isCancellationRequested()) fail(ErrorKind::Cancelled, "Operation cancelled");
        if (deadline_ && Clock::now() >= *deadline_) fail(ErrorKind::Timeout, "Operation timed out");
    }
    int waitMilliseconds() const {
        check();
        // Cancellation is cooperative: never wait more than 20 ms in the OS
        // when a token is present. Without a token or deadline wait indefinitely.
        int result = cancellation_.canBeCancelled() ? 20 : -1;
        if (deadline_) {
            const auto remaining = *deadline_ - Clock::now();
            auto count = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            if (count < 0) count = 0;
            if (count < INT_MAX) ++count; // Round upward, not a zero-time busy loop.
            const int bounded = static_cast<int>((std::min)(count, static_cast<decltype(count)>(INT_MAX)));
            if (result < 0 || bounded < result) result = bounded;
        }
        return result;
    }
private:
    using Clock = std::chrono::steady_clock;
    CancellationToken cancellation_;
    std::optional<Clock::time_point> deadline_;
};

inline void setNonBlocking(NativeSocket socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    if (::ioctlsocket(socket, FIONBIO, &enabled) != 0)
        systemError("ioctlsocket(FIONBIO)", lastError());
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    if (flags == -1 || ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == -1)
        systemError("fcntl(O_NONBLOCK)", lastError());
#endif
}

inline void waitReady(NativeSocket socket, bool writing, WaitContext& wait, bool connecting = false) {
    for (;;) {
        const int milliseconds = wait.waitMilliseconds();
#if defined(_WIN32)
        fd_set ready;
        fd_set errors;
        FD_ZERO(&ready);
        FD_ZERO(&errors);
        FD_SET(socket, &ready);
        FD_SET(socket, &errors);
        timeval timeout{};
        timeout.tv_sec = milliseconds < 0 ? 0 : milliseconds / 1000;
        timeout.tv_usec = milliseconds < 0 ? 0 : (milliseconds % 1000) * 1000;
        // Failed nonblocking connect is signalled in exceptfds on Winsock.
        const int result = ::select(0, writing ? nullptr : &ready,
            writing ? &ready : nullptr, connecting ? &errors : nullptr,
            milliseconds < 0 ? nullptr : &timeout);
#else
        (void)connecting;
        pollfd descriptor{};
        descriptor.fd = socket;
        descriptor.events = writing ? POLLOUT : POLLIN;
        const int result = ::poll(&descriptor, 1, milliseconds);
#endif
        if (result < 0) {
            const int code = lastError();
            if (interrupted(code)) continue;
            systemError("socket readiness wait", code);
        }
        wait.check();
        // Readiness, HUP and errors all lead to another native operation, which
        // supplies the actual result/EOF/error. Spurious readiness is retried.
        if (result > 0) return;
    }
}

inline void ensureRuntime() {
#if defined(_WIN32)
    struct WinsockRuntime {
        WinsockRuntime() {
            WSADATA data{};
            const int code = ::WSAStartup(MAKEWORD(2, 2), &data);
            if (code != 0) systemError("WSAStartup", code);
            if (data.wVersion != MAKEWORD(2, 2)) {
                ::WSACleanup();
                systemError("Winsock 2.2 unavailable", WSAVERNOTSUPPORTED);
            }
        }
        ~WinsockRuntime() { ::WSACleanup(); }
    };
    // One runtime per program, also when included by multiple translation units.
    static WinsockRuntime runtime;
    (void)runtime;
#endif
}

class SocketHandle {
public:
    SocketHandle() noexcept = default;
    explicit SocketHandle(NativeSocket socket) noexcept : socket_(socket) {}
    ~SocketHandle() { reset(); }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& other) noexcept
        : socket_(std::exchange(other.socket_, invalidSocket)) {}
    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            reset();
            socket_ = std::exchange(other.socket_, invalidSocket);
        }
        return *this;
    }
    bool isOpen() const noexcept { return socket_ != invalidSocket; }
    NativeSocket get() const noexcept { return socket_; }
    void reset() noexcept {
        const auto previous = std::exchange(socket_, invalidSocket);
        if (previous != invalidSocket) closeNative(previous);
    }
private:
    NativeSocket socket_ = invalidSocket;
};

inline void requireOpen(const SocketHandle& handle) {
    if (!handle.isOpen()) fail(ErrorKind::InvalidState, "Socket is closed or not connected");
}

inline void validateBuffer(const void* buffer, std::size_t size) {
    if (buffer == nullptr && size != 0)
        fail(ErrorKind::InvalidArgument, "A non-empty buffer must not be null");
}

inline void addProgress(std::size_t& progress, std::size_t count) noexcept {
    // Saturate for a maximum-size frame on a 32-bit size_t (prefix + payload).
    progress += (std::min)(count, (std::numeric_limits<std::size_t>::max)() - progress);
}

inline std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)>
resolve(const std::string& host, std::uint16_t port, int type, int family = AF_UNSPEC) {
    if (host.empty() || host.find('\0') != std::string::npos)
        fail(ErrorKind::InvalidArgument, "Host must be non-empty and contain no null characters");
    ensureRuntime();
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = type;
    hints.ai_protocol = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* result = nullptr;
    const auto service = std::to_string(port);
    const int code = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(result, &::freeaddrinfo);
    if (code != 0) {
#if defined(_WIN32)
        const auto description = std::system_category().message(code);
#else
        const std::string description = ::gai_strerror(code);
#endif
        throw NetworkError(ErrorKind::Resolution,
            "Cannot resolve '" + host + "': " + description +
            " (resolver code " + std::to_string(code) + ")", code);
    }
    if (!addresses) fail(ErrorKind::Resolution, "No compatible IP addresses found for '" + host + "'");
    return addresses;
}

inline NativeSocket createSocket(int type, int family = AF_INET) {
    ensureRuntime();
#if defined(_WIN32)
    const auto socket = ::socket(family, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
#else
    const auto socket = ::socket(family, type | SOCK_CLOEXEC,
        type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
#endif
    if (socket == invalidSocket) systemError("socket", lastError());
    return socket;
}

inline void validateIPv6Mode(IPv6Mode mode) {
    if (mode != IPv6Mode::IPv6Only && mode != IPv6Mode::DualStack)
        fail(ErrorKind::InvalidArgument, "Unknown IPv6 socket mode");
}

inline int nativeFamily(AddressFamily family) {
    switch (family) {
        case AddressFamily::Unspecified: return AF_UNSPEC;
        case AddressFamily::IPv4: return AF_INET;
        case AddressFamily::IPv6: return AF_INET6;
    }
    fail(ErrorKind::InvalidArgument, "Unknown address family");
}

inline void configureIPv6(NativeSocket socket, IPv6Mode mode) {
    const int onlyIPv6 = mode == IPv6Mode::IPv6Only ? 1 : 0;
#if defined(_WIN32)
    const auto* value = reinterpret_cast<const char*>(&onlyIPv6);
#else
    const auto* value = &onlyIPv6;
#endif
    if (::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, value, sizeof(onlyIPv6)) != 0)
        systemError("setsockopt(IPV6_V6ONLY)", lastError());
}

inline SocketHandle bindSocket(std::uint16_t port, const std::string& address, int type,
                               IPv6Mode mode = IPv6Mode::IPv6Only) {
    validateIPv6Mode(mode);
    auto addresses = resolve(address, port, type);
    int code = 0;
    bool compatible = false;
    for (auto* item = addresses.get(); item; item = item->ai_next) {
        if (item->ai_family != AF_INET && item->ai_family != AF_INET6) continue;
        if (mode == IPv6Mode::DualStack && item->ai_family != AF_INET6) continue;
        compatible = true;
        SocketHandle socket;
        try { socket = SocketHandle(createSocket(type, item->ai_family)); }
        catch (const NetworkError& error) {
            if (error.kind() != ErrorKind::System) throw;
            code = error.nativeCode();
            continue;
        }
        if (item->ai_family == AF_INET6) {
            // Set explicitly BEFORE bind: OS defaults differ.
            try { configureIPv6(socket.get(), mode); }
            catch (const NetworkError& error) {
                code = error.nativeCode();
                continue;
            }
        }
        const int enabled = 1;
#if defined(_WIN32)
        // Windows SO_REUSEADDR can steal an occupied port; use exclusive binding.
        if (::setsockopt(socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
            systemError("setsockopt(SO_EXCLUSIVEADDRUSE)", lastError());
#else
        // TCP restart after TIME_WAIT. UDP retains exclusive default binding.
        if (type == SOCK_STREAM && ::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR,
                &enabled, sizeof(enabled)) != 0)
            systemError("setsockopt(SO_REUSEADDR)", lastError());
#endif
        if (::bind(socket.get(), item->ai_addr,
                static_cast<AddressLength>(item->ai_addrlen)) == 0) {
            setNonBlocking(socket.get());
            return socket;
        }
        code = lastError();
    }
    if (!compatible)
        fail(ErrorKind::InvalidArgument, "DualStack requires an IPv6 bind address (normally ::)");
    systemError("bind", code);
}

inline sockaddr_storage socketAddress(const SocketHandle& handle, bool remote) {
    requireOpen(handle);
    sockaddr_storage address{};
    AddressLength length = sizeof(address);
    const int result = remote
        ? ::getpeername(handle.get(), reinterpret_cast<sockaddr*>(&address), &length)
        : ::getsockname(handle.get(), reinterpret_cast<sockaddr*>(&address), &length);
    if (result != 0) systemError(remote ? "getpeername" : "getsockname", lastError());
    return address;
}

inline bool isIPv4Mapped(const in6_addr& address) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&address);
    for (int index = 0; index < 10; ++index) if (bytes[index] != 0) return false;
    return bytes[10] == 0xff && bytes[11] == 0xff;
}

inline Endpoint formatEndpoint(const sockaddr_storage& storage) {
    Endpoint result;
    const void* bytes = nullptr;
    int family = storage.ss_family;
    sockaddr_in address4{};
    sockaddr_in6 address6{};
    if (family == AF_INET) {
        std::memcpy(&address4, &storage, sizeof(address4));
        bytes = &address4.sin_addr;
        result.port = ntohs(address4.sin_port);
        result.family = AddressFamily::IPv4;
    } else if (family == AF_INET6) {
        std::memcpy(&address6, &storage, sizeof(address6));
        result.port = ntohs(address6.sin6_port);
        if (isIPv4Mapped(address6.sin6_addr)) {
            // Present mapped peers as ordinary IPv4 so returned endpoints can
            // be reused with either an IPv4 socket or a dual-stack UDP socket.
            bytes = reinterpret_cast<const unsigned char*>(&address6.sin6_addr) + 12;
            family = AF_INET;
            result.family = AddressFamily::IPv4;
        } else {
            bytes = &address6.sin6_addr;
            result.family = AddressFamily::IPv6;
            result.scopeId = address6.sin6_scope_id;
        }
    } else {
        fail(ErrorKind::System, "Unsupported socket address family");
    }
    char text[INET6_ADDRSTRLEN]{};
    if (::inet_ntop(family, bytes, text, sizeof(text)) == nullptr)
        systemError("inet_ntop", lastError());
    result.address = text;
    if (result.scopeId != 0) result.address += "%" + std::to_string(result.scopeId);
    return result;
}

inline Endpoint endpoint(const SocketHandle& handle, bool remote) {
    return formatEndpoint(socketAddress(handle, remote));
}

struct Destination {
    sockaddr_storage address{};
    AddressLength length = 0;
};

inline Destination datagramDestination(const DatagramPacket& packet, int family, bool dualStack) {
    const int requestedFamily = nativeFamily(packet.family);
    if (!dualStack && requestedFamily != AF_UNSPEC && requestedFamily != family)
        fail(ErrorKind::InvalidArgument, "Destination family is incompatible with the bound UDP socket");
    const int filter = dualStack ? requestedFamily : family;
    auto addresses = resolve(packet.address, packet.port, SOCK_DGRAM, filter);
    Destination destination;
    const auto* address = addresses.get();
    if (address->ai_family == AF_INET && family == AF_INET6 && dualStack) {
        // Dual-stack Winsock requires a sockaddr_in6 even for IPv4 traffic.
        sockaddr_in source{};
        std::memcpy(&source, address->ai_addr, sizeof(source));
        sockaddr_in6 mapped{};
        mapped.sin6_family = AF_INET6;
        mapped.sin6_port = source.sin_port;
        auto* bytes = reinterpret_cast<unsigned char*>(&mapped.sin6_addr);
        bytes[10] = bytes[11] = 0xff;
        std::memcpy(bytes + 12, &source.sin_addr, sizeof(source.sin_addr));
        std::memcpy(&destination.address, &mapped, sizeof(mapped));
        destination.length = sizeof(mapped);
    } else {
        if (address->ai_family != family || address->ai_addrlen > sizeof(destination.address))
            fail(ErrorKind::InvalidArgument, "No destination compatible with the bound UDP socket");
        std::memcpy(&destination.address, address->ai_addr, address->ai_addrlen);
        destination.length = static_cast<AddressLength>(address->ai_addrlen);
    }
    if (family == AF_INET6) {
        sockaddr_in6 address6{};
        std::memcpy(&address6, &destination.address, sizeof(address6));
        const bool mapped = isIPv4Mapped(address6.sin6_addr);
        if (mapped && !dualStack)
            fail(ErrorKind::InvalidArgument, "IPv4-mapped UDP destination requires DualStack");
        if (mapped && (packet.scopeId != 0 || address6.sin6_scope_id != 0))
            fail(ErrorKind::InvalidArgument, "IPv4 destination must not have an IPv6 scope ID");
        if (packet.scopeId != 0) {
            if (address6.sin6_scope_id != 0 && address6.sin6_scope_id != packet.scopeId)
                fail(ErrorKind::InvalidArgument, "IPv6 scope ID conflicts with the address zone");
            address6.sin6_scope_id = packet.scopeId;
        }
        std::memcpy(&destination.address, &address6, sizeof(address6));
    } else if (packet.scopeId != 0) {
        fail(ErrorKind::InvalidArgument, "IPv4 destination must not have an IPv6 scope ID");
    }
    return destination;
}

} // namespace detail

class ServerSocket;

class Socket {
public:
    Socket() noexcept = default; // Empty: connect() can be called later.
    Socket(const std::string& host, std::uint16_t port, WaitOptions options = {},
           AddressFamily family = AddressFamily::Unspecified) { connect(host, port, options, family); }
    ~Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept
        : handle_(std::move(other.handle_)), inputEof_(std::exchange(other.inputEof_, false)),
          inputShutdown_(std::exchange(other.inputShutdown_, false)),
          outputShutdown_(std::exchange(other.outputShutdown_, false)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            handle_ = std::move(other.handle_);
            inputEof_ = std::exchange(other.inputEof_, false);
            inputShutdown_ = std::exchange(other.inputShutdown_, false);
            outputShutdown_ = std::exchange(other.outputShutdown_, false);
        }
        return *this;
    }

    void connect(const std::string& host, std::uint16_t port, WaitOptions options = {},
                 AddressFamily family = AddressFamily::Unspecified) {
        if (isOpen()) detail::fail(ErrorKind::InvalidState, "Socket is already connected");
        if (port == 0) detail::fail(ErrorKind::InvalidArgument, "Destination port must not be zero");
        detail::validateWaitOptions(options);
        // Portable synchronous getaddrinfo is not interruptible here. Socket
        // timeout starts AFTER resolution; cancellation is checked on both sides.
        auto addresses = detail::resolve(host, port, SOCK_STREAM, detail::nativeFamily(family));
        detail::WaitContext wait(options);
        int code = 0;
        for (auto* item = addresses.get(); item; item = item->ai_next) {
            wait.check();
            if (item->ai_family != AF_INET && item->ai_family != AF_INET6) continue;
            detail::SocketHandle candidate;
            try { candidate = detail::SocketHandle(detail::createSocket(SOCK_STREAM, item->ai_family)); }
            catch (const NetworkError& error) {
                if (error.kind() != ErrorKind::System) throw;
                code = error.nativeCode();
                continue;
            }
            if (item->ai_family == AF_INET6) {
                sockaddr_in6 destination{};
                std::memcpy(&destination, item->ai_addr, sizeof(destination));
                // Native IPv6 does not require IPv4 support. A mapped literal
                // explicitly opts into dual stack for this connected client.
                const auto mode = detail::isIPv4Mapped(destination.sin6_addr)
                    ? IPv6Mode::DualStack : IPv6Mode::IPv6Only;
                try { detail::configureIPv6(candidate.get(), mode); }
                catch (const NetworkError& error) { code = error.nativeCode(); continue; }
            }
            detail::setNonBlocking(candidate.get());
            const int result = ::connect(candidate.get(), item->ai_addr,
                static_cast<detail::AddressLength>(item->ai_addrlen));
            if (result != 0) {
                code = detail::lastError();
                if (!detail::connectPending(code)) continue;
                detail::waitReady(candidate.get(), true, wait, true);
                int connectionError = 0;
                detail::AddressLength length = sizeof(connectionError);
#if defined(_WIN32)
                auto* output = reinterpret_cast<char*>(&connectionError);
#else
                auto* output = &connectionError;
#endif
                if (::getsockopt(candidate.get(), SOL_SOCKET, SO_ERROR, output, &length) != 0)
                    detail::systemError("getsockopt(SO_ERROR)", detail::lastError());
                if (connectionError != 0) { code = connectionError; continue; }
            }
            wait.check();
            handle_ = std::move(candidate);
            inputEof_ = inputShutdown_ = outputShutdown_ = false;
            return;
        }
        wait.check();
        detail::systemError("connect", code);
    }

    bool isOpen() const noexcept { return handle_.isOpen(); }
    // Local established state, NOT a remote liveness probe. See hasReachedEof().
    bool isConnected() const noexcept { return isOpen(); }
    bool hasReachedEof() const noexcept { return inputEof_; }
    bool isInputShutdown() const noexcept { return inputShutdown_; }
    bool isOutputShutdown() const noexcept { return outputShutdown_; }
    void close() noexcept { handle_.reset(); inputEof_ = inputShutdown_ = outputShutdown_ = false; }
    Endpoint localEndpoint() const { return detail::endpoint(handle_, false); }
    Endpoint remoteEndpoint() const { return detail::endpoint(handle_, true); }

    // Explicit local half-close. Call on the owning thread, after cancelling
    // and joining any outstanding operation, not concurrently with I/O.
    void shutdownInput() { shutdownDirection(false); }
    void shutdownOutput() { shutdownDirection(true); }

    // May accept fewer bytes than size. Zero size is a no-op on an open socket.
    std::size_t sendSome(const void* data, std::size_t size, WaitOptions options = {}) {
        requireOutput();
        detail::validateBuffer(data, size);
        detail::WaitContext wait(options);
        std::size_t progress = 0;
        try { return sendSomeImpl(data, size, wait, progress); }
        catch (const NetworkError& error) { transferError(error, progress); }
        catch (...) { close(); throw; }
    }

    void sendAll(const void* data, std::size_t size, WaitOptions options = {}) {
        requireOutput();
        detail::validateBuffer(data, size);
        detail::WaitContext wait(options);
        std::size_t progress = 0;
        try { sendAllImpl(data, size, wait, progress); }
        catch (const NetworkError& error) { transferError(error, progress); }
        catch (...) { close(); throw; }
    }

    // Returns 1..capacity bytes, or 0 for orderly EOF. capacity must be > 0.
    std::size_t receive(void* buffer, std::size_t capacity, WaitOptions options = {}) {
        detail::requireOpen(handle_);
        detail::validateBuffer(buffer, capacity);
        if (capacity == 0) detail::fail(ErrorKind::InvalidArgument, "Receive capacity must be positive");
        detail::WaitContext wait(options);
        std::size_t progress = 0;
        try { return receiveImpl(buffer, capacity, wait, progress); }
        catch (const NetworkError& error) { transferError(error, progress); }
        catch (...) { close(); throw; }
    }

    // false: EOF before any byte; partial EOF: exception and close.
    // size == 0 succeeds without reading (including after EOF).
    bool receiveExact(void* buffer, std::size_t size, WaitOptions options = {}) {
        detail::requireOpen(handle_);
        detail::validateBuffer(buffer, size);
        detail::WaitContext wait(options);
        std::size_t progress = 0;
        try { return receiveExactImpl(buffer, size, wait, progress); }
        catch (const NetworkError& error) { transferError(error, progress); }
        catch (...) { close(); throw; }
    }

    // Wire format: 4-byte unsigned big-endian length, then exactly length bytes.
    void sendMessage(std::string_view message, std::size_t maxBytes = defaultMaxMessageSize,
                     WaitOptions options = {}) {
        requireOutput();
        if (message.size() > maxBytes || message.size() > (std::numeric_limits<std::uint32_t>::max)())
            detail::fail(ErrorKind::MessageTooLarge, "Message exceeds the configured or 32-bit wire limit");
        const auto length = static_cast<std::uint32_t>(message.size());
        const std::array<unsigned char, 4> prefix{{
            static_cast<unsigned char>(length >> 24), static_cast<unsigned char>(length >> 16),
            static_cast<unsigned char>(length >> 8), static_cast<unsigned char>(length)}};
        detail::WaitContext wait(options);
        std::size_t progress = 0;
        try {
            sendAllImpl(prefix.data(), prefix.size(), wait, progress);
            sendAllImpl(message.data(), message.size(), wait, progress);
        } catch (const NetworkError& error) { transferError(error, progress); }
        catch (...) { close(); throw; }
    }

    // nullopt means clean EOF BEFORE the next prefix. Empty string is a message.
    // A partially transferred frame is not recoverable: close on failure.
    std::optional<std::string> receiveMessage(std::size_t maxBytes = defaultMaxMessageSize,
                                            WaitOptions options = {}) {
        detail::requireOpen(handle_);
        detail::WaitContext wait(options);
        std::size_t progress = 0;
        try {
            std::array<unsigned char, 4> prefix{};
            if (!receiveExactImpl(prefix.data(), prefix.size(), wait, progress)) return std::nullopt;
            const std::uint32_t length = (std::uint32_t{prefix[0]} << 24) |
                (std::uint32_t{prefix[1]} << 16) | (std::uint32_t{prefix[2]} << 8) |
                std::uint32_t{prefix[3]};
            if (length > maxBytes)
                detail::fail(ErrorKind::MessageTooLarge, "Incoming message exceeds the configured limit");
            std::string message(length, '\0'); // Check the limit BEFORE allocating.
            if (!receiveExactImpl(message.data(), message.size(), wait, progress))
                detail::fail(ErrorKind::UnexpectedEof, "EOF before message payload");
            return message;
        } catch (const NetworkError& error) {
            transferError(error, progress);
        } catch (...) {
            if (progress != 0) close();
            throw;
        }
    }

private:
    friend class ServerSocket;
    explicit Socket(detail::SocketHandle handle) noexcept : handle_(std::move(handle)) {}

    void requireOutput() const {
        detail::requireOpen(handle_);
        if (outputShutdown_) detail::fail(ErrorKind::InvalidState, "Socket output has been shut down");
    }

    void shutdownDirection(bool output) {
        detail::requireOpen(handle_);
        auto& done = output ? outputShutdown_ : inputShutdown_;
        if (done) return;
#if defined(_WIN32)
        const int direction = output ? SD_SEND : SD_RECEIVE;
#else
        const int direction = output ? SHUT_WR : SHUT_RD;
#endif
        if (::shutdown(handle_.get(), direction) != 0) {
            const int code = detail::lastError();
            close();
            detail::systemError("shutdown (socket closed)", code);
        }
        done = true;
    }

    [[noreturn]] void transferError(const NetworkError& error, std::size_t progress) {
        const bool closing = progress != 0 || error.kind() == ErrorKind::System ||
            error.kind() == ErrorKind::UnexpectedEof;
        if (closing) close();
        throw NetworkError(error.kind(), std::string(error.what()) +
            (closing ? "; socket closed" : ""), error.nativeCode(), progress);
    }

    std::size_t sendSomeImpl(const void* data, std::size_t size,
                             detail::WaitContext& wait, std::size_t& progress) {
        wait.check();
        if (size == 0) return 0;
        const auto count = static_cast<int>((std::min)(size, detail::ioChunkSize));
        for (;;) {
            wait.check();
#if defined(_WIN32)
            const auto sent = ::send(handle_.get(), static_cast<const char*>(data), count, 0);
#else
            const auto sent = ::send(handle_.get(), data, static_cast<std::size_t>(count), MSG_NOSIGNAL);
#endif
            if (sent > 0) {
                detail::addProgress(progress, static_cast<std::size_t>(sent));
                return static_cast<std::size_t>(sent);
            }
            if (sent == 0) detail::fail(ErrorKind::System, "send made no progress");
            const int code = detail::lastError();
            if (detail::interrupted(code)) continue;
            if (detail::wouldBlock(code)) { detail::waitReady(handle_.get(), true, wait); continue; }
            detail::systemError("send", code);
        }
    }

    void sendAllImpl(const void* data, std::size_t size,
                     detail::WaitContext& wait, std::size_t& progress) {
        wait.check();
        const auto* bytes = static_cast<const char*>(data);
        std::size_t sent = 0;
        while (sent < size) sent += sendSomeImpl(bytes + sent, size - sent, wait, progress);
    }

    std::size_t receiveImpl(void* buffer, std::size_t capacity,
                            detail::WaitContext& wait, std::size_t& progress) {
        wait.check();
        if (inputEof_ || inputShutdown_) return 0;
        const auto count = static_cast<int>((std::min)(capacity, detail::ioChunkSize));
        for (;;) {
            wait.check();
#if defined(_WIN32)
            const auto received = ::recv(handle_.get(), static_cast<char*>(buffer), count, 0);
#else
            const auto received = ::recv(handle_.get(), buffer, static_cast<std::size_t>(count), 0);
#endif
            if (received >= 0) {
                if (received == 0) inputEof_ = true;
                detail::addProgress(progress, static_cast<std::size_t>(received));
                return static_cast<std::size_t>(received);
            }
            const int code = detail::lastError();
            if (detail::interrupted(code)) continue;
            if (detail::wouldBlock(code)) { detail::waitReady(handle_.get(), false, wait); continue; }
            detail::systemError("recv", code);
        }
    }

    bool receiveExactImpl(void* buffer, std::size_t size,
                          detail::WaitContext& wait, std::size_t& progress) {
        wait.check();
        auto* bytes = static_cast<char*>(buffer);
        std::size_t received = 0;
        while (received < size) {
            const auto count = receiveImpl(bytes + received, size - received, wait, progress);
            if (count == 0) {
                if (received == 0) return false;
                detail::fail(ErrorKind::UnexpectedEof, "EOF in the middle of an exact read");
            }
            received += count;
        }
        return true;
    }

    detail::SocketHandle handle_;
    bool inputEof_ = false;
    bool inputShutdown_ = false;
    bool outputShutdown_ = false;
};

class ServerSocket {
public:
    explicit ServerSocket(std::uint16_t port, const std::string& bindAddress = "0.0.0.0", int backlog = 16,
                          IPv6Mode mode = IPv6Mode::IPv6Only) {
        if (backlog <= 0) detail::fail(ErrorKind::InvalidArgument, "Listen backlog must be positive");
        auto socket = detail::bindSocket(port, bindAddress, SOCK_STREAM, mode);
        if (::listen(socket.get(), backlog) != 0) detail::systemError("listen", detail::lastError());
        handle_ = std::move(socket);
    }
    ~ServerSocket() = default;
    ServerSocket(const ServerSocket&) = delete;
    ServerSocket& operator=(const ServerSocket&) = delete;
    ServerSocket(ServerSocket&&) noexcept = default;
    ServerSocket& operator=(ServerSocket&&) noexcept = default;

    Socket accept(WaitOptions options = {}) {
        detail::requireOpen(handle_);
        detail::WaitContext wait(options);
        for (;;) {
            wait.check();
#if defined(_WIN32)
            const auto accepted = ::accept(handle_.get(), nullptr, nullptr);
#else
            const auto accepted = ::accept4(handle_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#endif
            if (accepted != detail::invalidSocket) {
                detail::SocketHandle socket(accepted);
                detail::setNonBlocking(socket.get());
                return Socket(std::move(socket));
            }
            const int code = detail::lastError();
            if (detail::interrupted(code)) continue;
            if (detail::wouldBlock(code)) { detail::waitReady(handle_.get(), false, wait); continue; }
            detail::systemError("accept", code);
        }
    }

    bool isOpen() const noexcept { return handle_.isOpen(); }
    void close() noexcept { handle_.reset(); }
    Endpoint localEndpoint() const { return detail::endpoint(handle_, false); }

private:
    detail::SocketHandle handle_;
};

class DatagramSocket {
public:
    // Port 0 asks the OS to choose a free local port, convenient for clients.
    explicit DatagramSocket(std::uint16_t port = 0, const std::string& bindAddress = "0.0.0.0",
                            IPv6Mode mode = IPv6Mode::IPv6Only)
        : handle_(detail::bindSocket(port, bindAddress, SOCK_DGRAM, mode)),
          family_(detail::socketAddress(handle_, false).ss_family),
          dualStack_(family_ == AF_INET6 && mode == IPv6Mode::DualStack) {}
    ~DatagramSocket() = default;
    DatagramSocket(const DatagramSocket&) = delete;
    DatagramSocket& operator=(const DatagramSocket&) = delete;
    DatagramSocket(DatagramSocket&&) noexcept = default;
    DatagramSocket& operator=(DatagramSocket&&) noexcept = default;

    bool isOpen() const noexcept { return handle_.isOpen(); }
    void close() noexcept { handle_.reset(); }
    Endpoint localEndpoint() const { return detail::endpoint(handle_, false); }

    void send(const DatagramPacket& packet, WaitOptions options = {}) {
        detail::requireOpen(handle_);
        if (packet.port == 0) detail::fail(ErrorKind::InvalidArgument, "Destination port must not be zero");
        if (packet.data.size() > maxDatagramSize)
            detail::fail(ErrorKind::MessageTooLarge, "UDP payload exceeds the NCL limit of 65507 bytes");
        detail::validateWaitOptions(options);
        const auto destination = detail::datagramDestination(packet, family_, dualStack_);
        detail::WaitContext wait(options); // Synchronous DNS is outside the timeout.
        // A datagram is sent once to the first compatible IP address. Do not
        // retry other addresses on apparent success: that could duplicate it.
        for (;;) {
            wait.check();
#if defined(_WIN32)
            const auto sent = ::sendto(handle_.get(), packet.data.data(),
                static_cast<int>(packet.data.size()), 0,
                reinterpret_cast<const sockaddr*>(&destination.address), destination.length);
#else
            const auto sent = ::sendto(handle_.get(), packet.data.data(), packet.data.size(),
                MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&destination.address), destination.length);
#endif
            if (sent >= 0) {
                if (static_cast<std::size_t>(sent) != packet.data.size())
                    detail::fail(ErrorKind::System, "Unexpected partial UDP send");
                return;
            }
            const int code = detail::lastError();
            if (detail::interrupted(code)) continue;
            if (detail::wouldBlock(code)) { detail::waitReady(handle_.get(), true, wait); continue; }
            detail::systemError("sendto", code);
        }
    }

    // Consumes ONE packet. If truncated, discard it and throw; never return
    // partial data as a complete packet. Zero-byte datagrams are valid.
    DatagramPacket receive(std::size_t capacity = maxDatagramSize, WaitOptions options = {}) {
        detail::requireOpen(handle_);
        if (capacity == 0 || capacity > maxDatagramSize)
            detail::fail(ErrorKind::InvalidArgument, "UDP receive capacity must be between 1 and 65507 bytes");
        detail::WaitContext wait(options);
        DatagramPacket packet;
        packet.data.resize(capacity);
        sockaddr_storage sender{};
        for (;;) {
            wait.check();
#if defined(_WIN32)
            int addressLength = sizeof(sender);
            const auto received = ::recvfrom(handle_.get(), packet.data.data(),
                static_cast<int>(capacity), 0, reinterpret_cast<sockaddr*>(&sender), &addressLength);
            if (received < 0) {
                const int code = detail::lastError();
                if (detail::interrupted(code)) continue;
                if (detail::wouldBlock(code)) { detail::waitReady(handle_.get(), false, wait); continue; }
                if (code == WSAEMSGSIZE)
                    detail::fail(ErrorKind::DatagramTruncated, "UDP packet exceeded the receive buffer and was discarded");
                detail::systemError("recvfrom", code);
            }
#else
            iovec buffer{};
            buffer.iov_base = packet.data.data();
            buffer.iov_len = capacity;
            msghdr message{};
            message.msg_name = &sender;
            message.msg_namelen = sizeof(sender);
            message.msg_iov = &buffer;
            message.msg_iovlen = 1;
            const auto received = ::recvmsg(handle_.get(), &message, 0);
            if (received < 0) {
                const int code = detail::lastError();
                if (detail::interrupted(code)) continue;
                if (detail::wouldBlock(code)) { detail::waitReady(handle_.get(), false, wait); continue; }
                detail::systemError("recvmsg", code);
            }
            if ((message.msg_flags & MSG_TRUNC) != 0)
                detail::fail(ErrorKind::DatagramTruncated, "UDP packet exceeded the receive buffer and was discarded");
#endif
            packet.data.resize(static_cast<std::size_t>(received));
            auto endpoint = detail::formatEndpoint(sender);
            packet.address = std::move(endpoint.address);
            packet.port = endpoint.port;
            packet.family = endpoint.family;
            packet.scopeId = endpoint.scopeId;
            return packet;
        }
    }

private:
    detail::SocketHandle handle_;
    int family_ = AF_INET;
    bool dualStack_ = false;
};

} // namespace ncl
#endif // NCL_HPP_INCLUDED
