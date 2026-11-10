 
 
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
#  error "NCL supports Windows and Linux only"
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
#include <vector>
#include <initializer_list>
#include <functional>
#include <ctime>
#include <cstdio>

 
#ifdef NCL_ENABLE_TLS
#  include <fstream>
#  include <type_traits>
#  ifdef _WIN32
#    pragma push_macro("small")
#    include <wincrypt.h>
#    pragma pop_macro("small")
#  else
#    include <sys/random.h>
#  endif
#endif

namespace ncl {

inline constexpr std::size_t defaultMaxMessageSize = 1024 * 1024;
 
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
    NetworkError(ErrorKind kind, const std::string& message, int nativeCode = 0, std::size_t bytesTransferred = 0)
        : std::runtime_error(message), kind_(kind), nativeCode_(nativeCode),
          bytesTransferred_(bytesTransferred) {}

    ErrorKind kind() const noexcept { return kind_; }
     
    int nativeCode() const noexcept { return nativeCode_; }
     
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
    std::string data;     
    std::string address;
    std::uint16_t port = 0;
    AddressFamily family = AddressFamily::Unspecified;  
    std::uint32_t scopeId = 0;  
};

namespace detail {

 
 
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
 
inline void closeNative(NativeSocket socket) noexcept { ::close(socket); }
#endif

[[noreturn]] inline void fail(ErrorKind kind, const std::string& message) {
    throw NetworkError(kind, message);
}

[[noreturn]] inline void systemError(const char* operation, int code) {
    throw NetworkError(ErrorKind::System, std::string(operation) + ": " + std::system_category().message(code) + " (native code " + std::to_string(code) + ")", code);
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
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
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
         
         
        int result = cancellation_.canBeCancelled() ? 20 : -1;
        if (deadline_) {
            const auto remaining = *deadline_ - Clock::now();
            auto count = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            if (count < 0) count = 0;
            if (count < INT_MAX) ++count;  
            const int bounded = static_cast<int>((std::min)(count, static_cast<decltype(count)>(INT_MAX)));
            if (result < 0 || bounded < result) result = bounded;
        }
        return result;
    }
    WaitContext boundedFor(std::chrono::milliseconds maximum) const {
        if (maximum.count() < 0) fail(ErrorKind::InvalidArgument, "Negative internal wait bound");
        auto result = *this;
        const auto now = Clock::now();
        const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
        const auto cap = maximum >= room ? Clock::time_point::max() : now + maximum;
        if (!result.deadline_ || cap < *result.deadline_) result.deadline_ = cap;
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
         
        const int result = ::select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, connecting ? &errors : nullptr, milliseconds < 0 ? nullptr : &timeout);
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
        throw NetworkError(ErrorKind::Resolution, "Cannot resolve '" + host + "': " + description + " (resolver code " + std::to_string(code) + ")", code);
    }
    if (!addresses) fail(ErrorKind::Resolution, "No compatible IP addresses found for '" + host + "'");
    return addresses;
}

inline NativeSocket createSocket(int type, int family = AF_INET) {
    ensureRuntime();
#if defined(_WIN32)
    const auto socket = ::socket(family, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
#else
    const auto socket = ::socket(family, type | SOCK_CLOEXEC, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
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

inline SocketHandle bindSocket(std::uint16_t port, const std::string& address, int type, IPv6Mode mode = IPv6Mode::IPv6Only) {
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
             
            try { configureIPv6(socket.get(), mode); }
            catch (const NetworkError& error) {
                code = error.nativeCode();
                continue;
            }
        }
        const int enabled = 1;
#if defined(_WIN32)
         
        if (::setsockopt(socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
            systemError("setsockopt(SO_EXCLUSIVEADDRUSE)", lastError());
#else
         
        if (type == SOCK_STREAM && ::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0)
            systemError("setsockopt(SO_REUSEADDR)", lastError());
#endif
        if (::bind(socket.get(), item->ai_addr, static_cast<AddressLength>(item->ai_addrlen)) == 0) {
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

}  

class ServerSocket;
namespace detail { class HttpTransport; }

class Socket {
public:
    Socket() noexcept = default;  
    Socket(const std::string& host, std::uint16_t port, WaitOptions options = {}, AddressFamily family = AddressFamily::Unspecified) { connect(host, port, options, family); }
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

    void connect(const std::string& host, std::uint16_t port, WaitOptions options = {}, AddressFamily family = AddressFamily::Unspecified) {
        if (isOpen()) detail::fail(ErrorKind::InvalidState, "Socket is already connected");
        if (port == 0) detail::fail(ErrorKind::InvalidArgument, "Destination port must not be zero");
        detail::validateWaitOptions(options);
         
         
        auto addresses = detail::resolve(host, port, SOCK_STREAM, detail::nativeFamily(family));
        detail::WaitContext wait(options);
        connectResolved(addresses.get(), wait);
    }

private:
    friend class detail::HttpTransport;
    void connectResolved(addrinfo* addresses, detail::WaitContext& wait) {
        int code = 0;
        for (auto* item = addresses; item; item = item->ai_next) {
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
                 
                 
                const auto mode = detail::isIPv4Mapped(destination.sin6_addr)
                    ? IPv6Mode::DualStack : IPv6Mode::IPv6Only;
                try { detail::configureIPv6(candidate.get(), mode); }
                catch (const NetworkError& error) { code = error.nativeCode(); continue; }
            }
            detail::setNonBlocking(candidate.get());
            const int result = ::connect(candidate.get(), item->ai_addr, static_cast<detail::AddressLength>(item->ai_addrlen));
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

public:
    bool isOpen() const noexcept { return handle_.isOpen(); }
     
    bool isConnected() const noexcept { return isOpen(); }
    bool hasReachedEof() const noexcept { return inputEof_; }
    bool isInputShutdown() const noexcept { return inputShutdown_; }
    bool isOutputShutdown() const noexcept { return outputShutdown_; }
    void close() noexcept { handle_.reset(); inputEof_ = inputShutdown_ = outputShutdown_ = false; }
    Endpoint localEndpoint() const { return detail::endpoint(handle_, false); }
    Endpoint remoteEndpoint() const { return detail::endpoint(handle_, true); }

     
     
    void shutdownInput() { shutdownDirection(false); }
    void shutdownOutput() { shutdownDirection(true); }

     
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

     
     
    bool receiveExact(void* buffer, std::size_t size, WaitOptions options = {}) {
        detail::requireOpen(handle_);
        detail::validateBuffer(buffer, size);
        detail::WaitContext wait(options);
        std::size_t progress = 0;
        try { return receiveExactImpl(buffer, size, wait, progress); }
        catch (const NetworkError& error) { transferError(error, progress); }
        catch (...) { close(); throw; }
    }

     
    void sendMessage(std::string_view message, std::size_t maxBytes = defaultMaxMessageSize, WaitOptions options = {}) {
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

     
     
    std::optional<std::string> receiveMessage(std::size_t maxBytes = defaultMaxMessageSize, WaitOptions options = {}) {
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
            std::string message(length, '\0');  
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
        throw NetworkError(error.kind(), std::string(error.what()) + (closing ? "; socket closed" : ""), error.nativeCode(), progress);
    }

    std::size_t sendSomeImpl(const void* data, std::size_t size, detail::WaitContext& wait, std::size_t& progress) {
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

    void sendAllImpl(const void* data, std::size_t size, detail::WaitContext& wait, std::size_t& progress) {
        wait.check();
        const auto* bytes = static_cast<const char*>(data);
        std::size_t sent = 0;
        while (sent < size) sent += sendSomeImpl(bytes + sent, size - sent, wait, progress);
    }

    std::size_t receiveImpl(void* buffer, std::size_t capacity, detail::WaitContext& wait, std::size_t& progress) {
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

    bool receiveExactImpl(void* buffer, std::size_t size, detail::WaitContext& wait, std::size_t& progress) {
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
    explicit ServerSocket(std::uint16_t port, const std::string& bindAddress = "0.0.0.0", int backlog = 16, IPv6Mode mode = IPv6Mode::IPv6Only) {
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
     
    explicit DatagramSocket(std::uint16_t port = 0, const std::string& bindAddress = "0.0.0.0", IPv6Mode mode = IPv6Mode::IPv6Only)
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
        detail::WaitContext wait(options);  
         
         
        for (;;) {
            wait.check();
#if defined(_WIN32)
            const auto sent = ::sendto(handle_.get(), packet.data.data(), static_cast<int>(packet.data.size()), 0, reinterpret_cast<const sockaddr*>(&destination.address), destination.length);
#else
            const auto sent = ::sendto(handle_.get(), packet.data.data(), packet.data.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&destination.address), destination.length);
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
            const auto received = ::recvfrom(handle_.get(), packet.data.data(), static_cast<int>(capacity), 0, reinterpret_cast<sockaddr*>(&sender), &addressLength);
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

enum class HttpErrorKind {
    InvalidArgument, InvalidUrl, InvalidResponse, HeadersTooLarge,
    BodyTooLarge, Unsupported, Tls, Certificate, InvalidRequest, UnsupportedVersion, ExpectationFailed
};

class HttpError : public std::runtime_error {
public:
    HttpError(HttpErrorKind kind, const std::string& message)
        : std::runtime_error(message), kind_(kind) {}
    HttpErrorKind kind() const noexcept { return kind_; }
private:
    HttpErrorKind kind_;
};

namespace detail {
[[noreturn]] inline void httpFail(HttpErrorKind kind, const char* message) {
    throw HttpError(kind, message);
}
inline char asciiLower(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}
inline bool httpEqual(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (asciiLower(left[i]) != asciiLower(right[i])) return false;
    return true;
}
inline bool tokenChar(unsigned char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') ||
        (value != 0 && std::strchr("!#$%&'*+-.^_`|~", value) != nullptr);
}
inline bool httpToken(std::string_view value) noexcept {
    if (value.empty()) return false;
    for (unsigned char ch : value) if (!tokenChar(ch)) return false;
    return true;
}
inline bool fieldValue(std::string_view value) noexcept {
    for (unsigned char ch : value) if ((ch < 32 && ch != '\t') || ch == 127) return false;
    return true;
}
inline std::string_view trimOws(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}
inline int hexDigit(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    value = asciiLower(value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}
inline std::size_t httpNumber(std::string_view text, unsigned base, HttpErrorKind kind) {
    if (text.empty()) httpFail(kind, "Empty HTTP size or port");
    std::size_t result = 0;
    for (char ch : text) {
        const int digit = base == 16 ? hexDigit(ch) : (ch >= '0' && ch <= '9' ? ch - '0' : -1);
        if (digit < 0 || static_cast<unsigned>(digit) >= base ||
            result > ((std::numeric_limits<std::size_t>::max)() - static_cast<unsigned>(digit)) / base)
            httpFail(kind, "Invalid or overflowing HTTP size or port");
        result = result * base + static_cast<unsigned>(digit);
    }
    return result;
}
}  

 
class HttpHeaders {
public:
    using Field = std::pair<std::string, std::string>;
    HttpHeaders() = default;
    HttpHeaders(std::initializer_list<Field> fields) {
        for (const auto& field : fields) add(field.first, field.second);
    }
    void add(std::string name, std::string value) {
        validate(name, value);
        fields_.emplace_back(std::move(name), std::move(value));
    }
    void set(std::string name, std::string value) {
        validate(name, value);
        erase(name);
        fields_.emplace_back(std::move(name), std::move(value));
    }
    void erase(std::string_view name) {
        fields_.erase(std::remove_if(fields_.begin(), fields_.end(), [&](const Field& field) {
            return detail::httpEqual(field.first, name);
        }), fields_.end());
    }
    std::optional<std::string> get(std::string_view name) const {
        for (const auto& field : fields_) if (detail::httpEqual(field.first, name)) return field.second;
        return std::nullopt;
    }
    std::vector<std::string> getAll(std::string_view name) const {
        std::vector<std::string> result;
        for (const auto& field : fields_) if (detail::httpEqual(field.first, name)) result.push_back(field.second);
        return result;
    }
    bool contains(std::string_view name) const {
        for (const auto& field : fields_) if (detail::httpEqual(field.first, name)) return true;
        return false;
    }
    std::size_t size() const noexcept { return fields_.size(); }
    auto begin() const noexcept { return fields_.begin(); }
    auto end() const noexcept { return fields_.end(); }
private:
    static void validate(std::string_view name, std::string_view value) {
        if (!detail::httpToken(name) || !detail::fieldValue(value))
            detail::httpFail(HttpErrorKind::InvalidArgument, "Invalid HTTP header name or value");
    }
    std::vector<Field> fields_;
};

struct HttpRequest {
    std::string method = "GET";
    HttpHeaders headers;
    std::string body;
};

struct HttpResponse {
    std::string version;
    int statusCode = 0;
    std::string reasonPhrase;
    HttpHeaders headers;
    std::string body;
    HttpHeaders trailers;
    static HttpResponse text(std::string content, int status = 200) {
        HttpResponse response;
        response.statusCode = status;
        response.headers.set("Content-Type", "text/plain; charset=utf-8");
        response.body = std::move(content);
        return response;
    }
     
    static HttpResponse json(std::string content, int status = 200) {
        auto response = text(std::move(content), status);
        response.headers.set("Content-Type", "application/json");
        return response;
    }
    static HttpResponse empty(int status = 204) {
        HttpResponse response;
        response.statusCode = status;
        return response;
    }
};

struct HttpServerRequest : HttpRequest {
    std::string version;
    std::string target;
    std::string path;
    std::string query;
    HttpHeaders trailers;
    Endpoint remote;
    std::vector<std::pair<std::string, std::string>> pathParameters;
    std::vector<std::pair<std::string, std::string>> queryParameters;
    std::vector<std::string> pathSegments;
    std::optional<std::string> pathParam(std::string_view name) const {
        for (const auto& field : pathParameters) if (field.first == name) return field.second;
        return std::nullopt;
    }
    std::optional<std::string> queryParam(std::string_view name) const {
        for (const auto& field : queryParameters) if (field.first == name) return field.second;
        return std::nullopt;
    }
    std::vector<std::string> queryParams(std::string_view name) const {
        std::vector<std::string> values;
        for (const auto& field : queryParameters) if (field.first == name) values.push_back(field.second);
        return values;
    }
};

struct HttpRouteOptions {
    std::string summary;
    std::string requestContentType;
    std::string responseContentType = "application/json";
    int successStatus = 200;
    bool requestBodyRequired = false;
    std::vector<std::string> queryParameters;
};

struct HttpOptions {
    WaitOptions wait{};
    std::size_t maxHeaderBytes = 32 * 1024;
    std::size_t maxBodyBytes = 8 * 1024 * 1024;
    std::size_t maxChunkMetadataBytes = 256 * 1024;
    std::size_t maxInformationalResponses = 8;
     
    std::string caFile;
    AddressFamily family = AddressFamily::Unspecified;
};

namespace detail {
struct HttpUrl {
    bool secure = false;
    bool numeric = false;
    std::string host;
    std::string authority;
    std::string target;
    std::uint16_t port = 80;
};

inline HttpUrl parseHttpUrl(std::string_view text) {
    HttpUrl url;
    for (unsigned char ch : text)
        if (ch <= 32 || ch >= 127 || ch == '\\')
            httpFail(HttpErrorKind::InvalidUrl, "URL must be ASCII without spaces or control characters; encode its path");
    const auto separator = text.find("://");
    if (separator == std::string_view::npos) httpFail(HttpErrorKind::InvalidUrl, "Expected an absolute http:// or https:// URL");
    if (httpEqual(text.substr(0, separator), "https")) { url.secure = true; url.port = 443; }
    else if (!httpEqual(text.substr(0, separator), "http")) httpFail(HttpErrorKind::InvalidUrl, "Unsupported URL scheme");
    text.remove_prefix(separator + 3);
    const auto end = text.find_first_of("/?#");
    const auto authority = text.substr(0, end);
    if (authority.empty() || authority.find('@') != std::string_view::npos)
        httpFail(HttpErrorKind::InvalidUrl, "URL needs a host; credentials in URLs are not supported");
    std::string_view host;
    std::string_view port;
    bool explicitPort = false;
    if (authority.front() == '[') {
        const auto bracket = authority.find(']');
        if (bracket == std::string_view::npos) httpFail(HttpErrorKind::InvalidUrl, "Unclosed IPv6 URL address");
        host = authority.substr(1, bracket - 1);
        in6_addr address{};
        ensureRuntime();
        if (::inet_pton(AF_INET6, std::string(host).c_str(), &address) != 1)
            httpFail(HttpErrorKind::InvalidUrl, "Invalid IPv6 URL address; scoped literals are not supported");
        url.numeric = true;
        if (bracket + 1 < authority.size()) {
            if (authority[bracket + 1] != ':') httpFail(HttpErrorKind::InvalidUrl, "Unexpected text after IPv6 URL address");
            explicitPort = true;
            port = authority.substr(bracket + 2);
        }
        url.authority = "[" + std::string(host) + "]";
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (host.empty()) httpFail(HttpErrorKind::InvalidUrl, "Empty URL host");
        for (unsigned char ch : host)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '.'))
                httpFail(HttpErrorKind::InvalidUrl, "Invalid URL hostname; IPv6 literals require brackets");
        if (colon != std::string_view::npos) { explicitPort = true; port = authority.substr(colon + 1); }
        in_addr address{};
        ensureRuntime();
        url.numeric = ::inet_pton(AF_INET, std::string(host).c_str(), &address) == 1;
        url.authority = std::string(host);
    }
    url.host = std::string(host);
    if (explicitPort) {
        const auto value = httpNumber(port, 10, HttpErrorKind::InvalidUrl);
        if (value == 0 || value > 65535) httpFail(HttpErrorKind::InvalidUrl, "URL port must be between 1 and 65535");
        url.port = static_cast<std::uint16_t>(value);
        url.authority += ":" + std::to_string(value);
    }
    auto target = end == std::string_view::npos ? std::string_view{} : text.substr(end);
    target = target.substr(0, target.find('#'));  
    url.target = target.empty() || target.front() == '?' ? "/" + std::string(target) : std::string(target);
    for (std::size_t i = 0; i < url.target.size(); ++i) {
        if (url.target[i] == '%') {
            if (i + 2 >= url.target.size() || hexDigit(url.target[i + 1]) < 0 || hexDigit(url.target[i + 2]) < 0)
                httpFail(HttpErrorKind::InvalidUrl, "Invalid percent escape in URL");
            i += 2;
        }
    }
    return url;
}

inline std::string serializeHttpRequest(const HttpUrl& url, const HttpRequest& request, const HttpOptions& options) {
    if (!httpToken(request.method)) httpFail(HttpErrorKind::InvalidArgument, "HTTP method must be a non-empty token");
    if (request.method == "CONNECT") httpFail(HttpErrorKind::Unsupported, "HTTP CONNECT tunnels are not supported");
    if (request.body.size() > options.maxBodyBytes) httpFail(HttpErrorKind::BodyTooLarge, "HTTP request body exceeds the configured limit");
    std::string result;
    const auto append = [&](std::string_view value) {
        if (value.size() > options.maxHeaderBytes - result.size())
            httpFail(HttpErrorKind::HeadersTooLarge, "HTTP request headers exceed the configured limit");
        result.append(value.data(), value.size());
    };
    append(request.method); append(" "); append(url.target); append(" HTTP/1.1\r\nHost: ");
    append(url.authority); append("\r\nConnection: close\r\n");
    for (const auto& field : request.headers) {
        for (auto managed : {"Host", "Content-Length", "Transfer-Encoding", "Connection", "Expect", "Upgrade", "Trailer", "TE"})
            if (httpEqual(field.first, managed))
                httpFail(HttpErrorKind::InvalidArgument, "This HTTP header is managed by NCL or requires an unsupported feature");
        append(field.first); append(": "); append(field.second); append("\r\n");
    }
    if (!request.headers.contains("Accept-Encoding")) append("Accept-Encoding: identity\r\n");
    if (!request.body.empty() || request.method == "POST" || request.method == "PUT" || request.method == "PATCH") {
        append("Content-Length: "); append(std::to_string(request.body.size())); append("\r\n");
    }
    append("\r\n");
    return result;
}

inline std::string decodeHttpComponent(std::string_view text, bool plus = false, bool path = false) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == '%') {
            if (i + 2 >= text.size() || hexDigit(text[i + 1]) < 0 || hexDigit(text[i + 2]) < 0)
                httpFail(HttpErrorKind::InvalidRequest, "Invalid percent escape in HTTP request target");
            ch = static_cast<unsigned char>(hexDigit(text[i + 1]) * 16 + hexDigit(text[i + 2]));
            i += 2;
        } else if (ch == '+' && plus) ch = ' ';
        if (ch < 32 || ch == 127 || (path && (ch == '/' || ch == '\\')))
            httpFail(HttpErrorKind::InvalidRequest, "Forbidden decoded character in HTTP request target");
        result += static_cast<char>(ch);
    }
    return result;
}
inline std::vector<std::string> httpPathSegments(std::string_view path) {
    std::vector<std::string> result;
    if (path == "/") return result;
    path.remove_prefix(1);
    for (;;) {
        const auto slash = path.find('/');
        auto segment = decodeHttpComponent(path.substr(0, slash), false, true);
        if (segment == "." || segment == "..") httpFail(HttpErrorKind::InvalidRequest, "Dot segments are not accepted in HTTP routes");
        result.push_back(std::move(segment));
        if (slash == std::string_view::npos) return result;
        path.remove_prefix(slash + 1);
    }
}
inline void parseServerTarget(HttpServerRequest& request) {
    if (request.target == "*" && request.method == "OPTIONS") { request.path = "*"; return; }
    if (request.target.empty() || request.target.front() != '/')
        httpFail(HttpErrorKind::InvalidRequest, "Only origin-form HTTP request targets are supported");
    for (unsigned char ch : request.target)
        if (ch <= 32 || ch >= 127 || ch == '#' || ch == '\\')
            httpFail(HttpErrorKind::InvalidRequest, "Invalid HTTP request target");
    const auto separator = request.target.find('?');
    const auto path = std::string_view(request.target).substr(0, separator);
    request.path = "/";
    const auto& segments = request.pathSegments = httpPathSegments(path);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) request.path += '/';
        request.path += segments[i];
    }
    if (separator == std::string::npos) return;
    request.query = request.target.substr(separator + 1);
    std::string_view query = request.query;
    while (!query.empty()) {
        const auto ampersand = query.find('&');
        const auto pair = query.substr(0, ampersand);
        if (!pair.empty()) {
            const auto equals = pair.find('=');
            request.queryParameters.emplace_back(decodeHttpComponent(pair.substr(0, equals), true), equals == std::string_view::npos ? "" : decodeHttpComponent(pair.substr(equals + 1), true));
        }
        if (ampersand == std::string_view::npos) break;
        query.remove_prefix(ampersand + 1);
    }
}
 
} }  
#ifdef NCL_ENABLE_TLS
 
namespace ncl { namespace detail { namespace tls {

using Byte = unsigned char;
template<class T> class ErasingAllocator {
public:
    using value_type=T;
    using propagate_on_container_move_assignment=std::true_type;
    using is_always_equal=std::true_type;
    ErasingAllocator() noexcept=default;
    template<class U> ErasingAllocator(const ErasingAllocator<U>&) noexcept {}
    T* allocate(std::size_t count) { return std::allocator<T>{}.allocate(count); }
    void deallocate(T* data,std::size_t count) noexcept {
        volatile unsigned char* bytes=reinterpret_cast<volatile unsigned char*>(data);
        for (std::size_t i=0;i<count*sizeof(T);++i) bytes[i]=0;
        std::allocator<T>{}.deallocate(data,count);
    }
};
template<class T,class U> inline bool operator==(const ErasingAllocator<T>&,const ErasingAllocator<U>&) noexcept { return true; }
template<class T,class U> inline bool operator!=(const ErasingAllocator<T>&,const ErasingAllocator<U>&) noexcept { return false; }
using Bytes = std::vector<Byte,ErasingAllocator<Byte>>;

[[noreturn]] inline void fail(const std::string& message, HttpErrorKind kind = HttpErrorKind::Tls) {
    throw HttpError(kind, "NCL TLS: " + message);
}

inline void eraseSecret(void* data, std::size_t size) noexcept {
    volatile Byte* bytes = static_cast<volatile Byte*>(data);
    while (size--) *bytes++ = 0;
}
inline void eraseSecret(Bytes& value) noexcept { eraseSecret(value.data(), value.size()); }

struct View {
    const Byte* data = nullptr;
    std::size_t size = 0;
    View() = default;
    View(const Byte* p, std::size_t n) : data(p), size(n) {}
    View(const Bytes& b) : data(b.data()), size(b.size()) {}
    View(std::string_view s) : data(reinterpret_cast<const Byte*>(s.data())), size(s.size()) {}
    template<std::size_t N> View(const std::array<Byte, N>& b) : data(b.data()), size(N) {}
    View sub(std::size_t offset, std::size_t count) const {
        if (offset > size || count > size - offset) fail("truncated encoding");
        return {offset ? data + offset : data, count};
    }
    Bytes copy() const { return size ? Bytes(data, data + size) : Bytes{}; }
};
inline void append(Bytes& out, View in) {
    if (in.size) out.insert(out.end(), in.data, in.data + in.size);
}
inline bool equal(View a, View b) noexcept {
    if (a.size != b.size) return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < a.size; ++i) difference |= a.data[i] ^ b.data[i];
    return difference == 0;
}
inline std::uint32_t be32(const Byte* p) noexcept {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
        (std::uint32_t(p[2]) << 8) | p[3];
}
inline std::uint64_t be64(const Byte* p) noexcept {
    return (std::uint64_t(be32(p)) << 32) | be32(p + 4);
}
inline void put32(Byte* p, std::uint32_t x) noexcept {
    for (unsigned i = 0; i != 4; ++i) p[i] = static_cast<Byte>(x >> (24 - 8 * i));
}
inline void put64(Byte* p, std::uint64_t x) noexcept {
    for (unsigned i = 0; i != 8; ++i) p[i] = static_cast<Byte>(x >> (56 - 8 * i));
}
inline Bytes number(std::uint64_t n, std::size_t size) {
    Bytes out(size);
    for (std::size_t i = size; i != 0; --i) { out[i - 1] = static_cast<Byte>(n); n >>= 8; }
    if (n) fail("integer does not fit its encoding");
    return out;
}
inline void integer(Bytes& out, std::uint64_t n, std::size_t size) { append(out, number(n, size)); }
inline Bytes vector(View value, unsigned prefix) {
    Bytes out = number(value.size, prefix); append(out, value); return out;
}

class Reader {
public:
    explicit Reader(View view) : view_(view) {}
    std::size_t remaining() const noexcept { return view_.size - position_; }
    View take(std::size_t size) {
        const auto result = view_.sub(position_, size); position_ += size; return result;
    }
    std::uint64_t uint(unsigned size) {
        if (size > 8) fail("oversized integer");
        const auto b = take(size); std::uint64_t result = 0;
        for (std::size_t i = 0; i < b.size; ++i) result = (result << 8) | b.data[i];
        return result;
    }
    View vec(unsigned prefix) { const auto n = uint(prefix); return take(static_cast<std::size_t>(n)); }
    void end() const { if (remaining()) fail("trailing bytes in encoding"); }
private:
    View view_;
    std::size_t position_ = 0;
};

inline Bytes random(std::size_t size) {
    Bytes out(size);
#if defined(_WIN32)
     
     
    using Generate = LONG (WINAPI*)(void*, unsigned char*, unsigned long, unsigned long);
    struct Provider {
        HMODULE module = ::LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        Generate generate = nullptr;
        Provider() {
            const auto address=module ? ::GetProcAddress(module,"BCryptGenRandom") : nullptr;
            static_assert(sizeof(generate)==sizeof(address),"Windows function pointer size");
            std::memcpy(&generate,&address,sizeof(generate));
        }
        ~Provider() { if (module) ::FreeLibrary(module); }
    };
     
    static const Provider provider;
    if (!provider.generate || size > ULONG_MAX || provider.generate(nullptr, out.data(), static_cast<ULONG>(size), 2) < 0)
        fail("operating-system random generator failed");
#else
    std::size_t done = 0;
    while (done != size) {
        const auto n = ::getrandom(out.data() + done, size - done, 0);
        if (n < 0) { if (errno == EINTR) continue; fail("operating-system random generator failed"); }
        if (n == 0) fail("operating-system random generator made no progress");
        done += static_cast<std::size_t>(n);
    }
#endif
    return out;
}

class Sha256 {
public:
    void update(View input) {
        if (input.size > (UINT64_MAX / 8) - bytes_) fail("SHA-256 input is too long");
        bytes_ += input.size;
        std::size_t offset = 0;
        while (offset < input.size) {
            const auto count = (std::min)(64 - used_, input.size - offset);
            std::memcpy(buffer_.data() + used_, input.data + offset, count);
            used_ += count; offset += count;
            if (used_ == 64) { block(buffer_.data()); used_ = 0; }
        }
    }
    Bytes finish() const {
        auto copy = *this;
        copy.buffer_[copy.used_++] = 0x80;
        if (copy.used_ > 56) {
            std::fill(copy.buffer_.begin() + copy.used_, copy.buffer_.end(), Byte{0});
            copy.block(copy.buffer_.data()); copy.used_ = 0;
        }
        std::fill(copy.buffer_.begin() + copy.used_, copy.buffer_.begin() + 56, Byte{0});
        put64(copy.buffer_.data() + 56, copy.bytes_ * 8); copy.block(copy.buffer_.data());
        Bytes result(32);
        for (unsigned i = 0; i < 8; ++i) put32(result.data() + 4 * i, copy.state_[i]);
        eraseSecret(&copy, sizeof(copy)); return result;
    }
    static Bytes hash(View input) { Sha256 h; h.update(input); return h.finish(); }
private:
    static std::uint32_t rotate(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32 - n)); }
    void block(const Byte* input) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64];
        for (unsigned i = 0; i < 16; ++i) w[i] = be32(input + 4 * i);
        for (unsigned i = 16; i < 64; ++i) {
            const auto a = w[i - 15], b = w[i - 2];
            w[i] = w[i - 16] + (rotate(a,7)^rotate(a,18)^(a>>3)) + w[i - 7] + (rotate(b,17)^rotate(b,19)^(b>>10));
        }
        auto a=state_[0], b=state_[1], c=state_[2], d=state_[3], e=state_[4], f=state_[5], g=state_[6], h=state_[7];
        for (unsigned i = 0; i < 64; ++i) {
            const auto t1 = h + (rotate(e,6)^rotate(e,11)^rotate(e,25)) + ((e&f)^(~e&g)) + k[i] + w[i];
            const auto t2 = (rotate(a,2)^rotate(a,13)^rotate(a,22)) + ((a&b)^(a&c)^(b&c));
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
        eraseSecret(w, sizeof(w));
    }
    std::array<std::uint32_t,8> state_{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    std::array<Byte,64> buffer_{};
    std::size_t used_ = 0;
    std::uint64_t bytes_ = 0;
};

inline Bytes hmac(View key, View message) {
    Bytes normalized = key.size > 64 ? Sha256::hash(key) : key.copy(); normalized.resize(64);
    Bytes pad(64);
    for (unsigned i=0;i<64;++i) pad[i] = normalized[i] ^ 0x36;
    Sha256 inner; inner.update(pad); inner.update(message); auto digest=inner.finish();
    for (unsigned i=0;i<64;++i) pad[i] = normalized[i] ^ 0x5c;
    Sha256 outer; outer.update(pad); outer.update(digest); auto result=outer.finish();
    eraseSecret(normalized); eraseSecret(pad); eraseSecret(digest);
    eraseSecret(&inner,sizeof(inner)); eraseSecret(&outer,sizeof(outer)); return result;
}
inline Bytes expand(View key, View info, std::size_t length) {
    if (length > 255*32) fail("HKDF output is too long");
    Bytes result, previous;
    for (unsigned i=1;result.size()<length;++i) {
        Bytes input=previous; append(input,info); input.push_back(static_cast<Byte>(i));
        eraseSecret(previous); previous=hmac(key,input); eraseSecret(input);
        append(result,View(previous).sub(0,(std::min)(previous.size(),length-result.size())));
    }
    eraseSecret(previous); return result;
}
inline Bytes label(View secret, std::string_view name, View context, std::size_t length) {
    const std::string full="tls13 "+std::string(name);
    if (full.size()>255 || context.size>255 || length>65535) fail("invalid TLS HKDF label");
    Bytes info=number(length,2); append(info,vector(View(full),1)); append(info,vector(context,1));
    return expand(secret,info,length);
}
inline Bytes derive(View secret, std::string_view name, View transcript) {
    return label(secret,name,Sha256::hash(transcript),32);
}
inline Bytes prf(View key, std::string_view name, View seed, std::size_t length) {
    Bytes input; append(input,View(name)); append(input,seed);
    Bytes a=hmac(key,input), out;
    while (out.size()<length) {
        Bytes block=a; append(block,input); auto digest=hmac(key,block);
        append(out,View(digest).sub(0,(std::min)(digest.size(),length-out.size())));
        auto next=hmac(key,a); eraseSecret(a); a=std::move(next);
        eraseSecret(block); eraseSecret(digest);
    }
    eraseSecret(a); return out;
}

 
inline Byte gfMultiply(Byte a, Byte b) noexcept {
    Byte result=0;
    for (unsigned i=0;i<8;++i) {
        result ^= static_cast<Byte>(a & (0u - (b&1u)));
        a=static_cast<Byte>((a<<1) ^ (0x1bu & (0u-(a>>7)))); b>>=1;
    }
    return result;
}
inline Byte aesSub(Byte x) noexcept {
    Byte inverse=1, power=x;
    for (unsigned i=0;i<8;++i) {
        if ((254u>>i)&1u) inverse=gfMultiply(inverse,power);
        power=gfMultiply(power,power);
    }
    const auto rotate=[](Byte a,unsigned n) { return static_cast<Byte>((a<<n)|(a>>(8-n))); };
    return static_cast<Byte>(inverse^rotate(inverse,1)^rotate(inverse,2)^rotate(inverse,3)^rotate(inverse,4)^0x63);
}
class Aes128 {
public:
    explicit Aes128(View key) {
        if (key.size!=16) fail("AES-128 needs a 16-byte key");
        std::copy(key.data,key.data+16,keys_.begin()); Byte round=1;
        for (unsigned offset=16;offset<176;offset+=4) {
            std::array<Byte,4> temp{}; std::copy_n(keys_.data()+offset-4,4,temp.begin());
            if (offset%16==0) {
                const auto first=temp[0];
                temp={{static_cast<Byte>(aesSub(temp[1])^round),aesSub(temp[2]),aesSub(temp[3]),aesSub(first)}};
                round=gfMultiply(round,2);
            }
            for (unsigned i=0;i<4;++i) keys_[offset+i]=keys_[offset+i-16]^temp[i];
        }
    }
    ~Aes128() { eraseSecret(keys_.data(),keys_.size()); }
    std::array<Byte,16> encrypt(const std::array<Byte,16>& input) const {
        auto state=input;
        for (unsigned i=0;i<16;++i) state[i]^=keys_[i];
        for (unsigned round=1;round<=10;++round) {
            std::array<Byte,16> shifted{};
            for (unsigned col=0;col<4;++col) for (unsigned row=0;row<4;++row)
                shifted[4*col+row]=aesSub(state[4*((col+row)%4)+row]);
            state=shifted;
            if (round!=10) for (unsigned col=0;col<4;++col) {
                const unsigned j=4*col; const Byte a=state[j],b=state[j+1],c=state[j+2],d=state[j+3];
                const Byte sum=a^b^c^d;
                state[j]^=sum^gfMultiply(a^b,2); state[j+1]^=sum^gfMultiply(b^c,2);
                state[j+2]^=sum^gfMultiply(c^d,2); state[j+3]^=sum^gfMultiply(d^a,2);
            }
            for (unsigned i=0;i<16;++i) state[i]^=keys_[16*round+i];
        }
        return state;
    }
private:
    std::array<Byte,176> keys_{};
};

inline std::array<Byte,16> galois(const std::array<Byte,16>& x, const std::array<Byte,16>& h) noexcept {
    std::array<Byte,16> z{}; auto v=h;
    for (unsigned bit=0;bit<128;++bit) {
        const Byte mask=static_cast<Byte>(0u-((x[bit/8]>>(7-bit%8))&1u));
        for (unsigned i=0;i<16;++i) z[i]^=v[i]&mask;
        const Byte reduction=static_cast<Byte>(0xe1u & (0u-(v[15]&1u)));
        for (unsigned i=15;i>0;--i) v[i]=static_cast<Byte>((v[i]>>1)|(v[i-1]<<7));
        v[0]=static_cast<Byte>((v[0]>>1)^reduction);
    }
    return z;
}
inline Bytes gcm(View key, View nonce, View aad, View input, bool decrypt) {
    if (nonce.size!=12 || (decrypt && input.size<16)) fail("invalid AES-GCM encoding");
    const std::size_t count=input.size-(decrypt?16:0);
    if (count>16384+256 || aad.size>65536) fail("AES-GCM record exceeds bounds");
    const Aes128 aes(key);
    const std::array<Byte,16> zero{}; const auto h=aes.encrypt(zero);
    std::array<Byte,16> j{}; std::copy_n(nonce.data,12,j.begin()); j[15]=1;
    auto counter=j; Bytes output(count+(decrypt?0:16));
    for (std::size_t offset=0;offset<count;offset+=16) {
        for (unsigned i=16;i-->12;) if (++counter[i]) break;
        auto stream=aes.encrypt(counter); const auto n=(std::min)(std::size_t{16},count-offset);
        for (std::size_t i=0;i<n;++i) output[offset+i]=input.data[offset+i]^stream[i];
        eraseSecret(stream.data(),stream.size());
    }
    std::array<Byte,16> accumulator{};
    const auto absorb=[&](View data) {
        for (std::size_t offset=0;offset<data.size;offset+=16) {
            const auto n=(std::min)(std::size_t{16},data.size-offset);
            for (std::size_t i=0;i<n;++i) accumulator[i]^=data.data[offset+i];
            accumulator=galois(accumulator,h);
        }
    };
    absorb(aad); absorb(decrypt?input.sub(0,count):View(output).sub(0,count));
    std::array<Byte,16> lengths{}; put64(lengths.data(),aad.size*8); put64(lengths.data()+8,count*8);
    for (unsigned i=0;i<16;++i) accumulator[i]^=lengths[i];
    accumulator=galois(accumulator,h); const auto mask=aes.encrypt(j);
    for (unsigned i=0;i<16;++i) accumulator[i]^=mask[i];
    if (decrypt) {
        if (!equal(accumulator,input.sub(count,16))) { eraseSecret(output); fail("record authentication failed"); }
    } else std::copy(accumulator.begin(),accumulator.end(),output.begin()+count);
    return output;
}

struct Field25519 {
    std::array<std::uint64_t,16> word{};
    explicit Field25519(std::uint64_t initialValue=0) { word[0]=initialValue; }
    void normalize() noexcept {
        for (unsigned round=0;round<4;++round) {
            for (unsigned i=0;i<15;++i) { word[i+1]+=word[i]>>16; word[i]&=65535; }
            const auto carry=word[15]>>15; word[15]&=32767; word[0]+=19*carry;
        }
        std::array<std::uint64_t,16> reduced{}; std::uint64_t borrow=0;
        for (unsigned i=0;i<16;++i) {
            const auto prime=i==0?65517u:i==15?32767u:65535u;
            const auto value=word[i]-prime-borrow; borrow=value>>63;
            reduced[i]=value&(i==15?32767u:65535u);
        }
        const auto mask=std::uint64_t{0}-(1-borrow);
        for (unsigned i=0;i<16;++i) word[i]=(word[i]&~mask)|(reduced[i]&mask);
    }
    static Field25519 from(View bytes) {
        if (bytes.size!=32) fail("X25519 needs a 32-byte coordinate");
        Field25519 f;
        for (unsigned i=0;i<16;++i) f.word[i]=bytes.data[2*i]|(std::uint64_t(bytes.data[2*i+1])<<8);
        f.word[15]&=32767; f.normalize(); return f;
    }
    Bytes bytes() const {
        auto f=*this; f.normalize(); Bytes out(32);
        for (unsigned i=0;i<16;++i) { out[2*i]=static_cast<Byte>(f.word[i]); out[2*i+1]=static_cast<Byte>(f.word[i]>>8); }
        eraseSecret(&f,sizeof(f)); return out;
    }
};
inline Field25519 operator+(const Field25519& a,const Field25519& b) {
    Field25519 c; for (unsigned i=0;i<16;++i) c.word[i]=a.word[i]+b.word[i]; c.normalize(); return c;
}
inline Field25519 operator-(const Field25519& a,const Field25519& b) {
    Field25519 c;
    for (unsigned i=0;i<16;++i) c.word[i]=a.word[i]+2*(i==0?65517u:i==15?32767u:65535u)-b.word[i];
    c.normalize(); return c;
}
inline Field25519 operator*(const Field25519& a,const Field25519& b) {
    std::array<std::uint64_t,32> product{};
    for (unsigned i=0;i<16;++i) for (unsigned j=0;j<16;++j) product[i+j]+=a.word[i]*b.word[j];
    for (unsigned i=16;i<32;++i) product[i-16]+=38*product[i];
    Field25519 c; std::copy_n(product.begin(),16,c.word.begin()); c.normalize();
    eraseSecret(product.data(),sizeof(product)); return c;
}
inline void swap(Field25519& a,Field25519& b,unsigned bit) noexcept {
    const auto mask=std::uint64_t{0}-bit;
    for (unsigned i=0;i<16;++i) { const auto d=(a.word[i]^b.word[i])&mask; a.word[i]^=d; b.word[i]^=d; }
}
inline Field25519 inverse(const Field25519& input) {
    Field25519 value(1);
    for (int bit=254;bit>=0;--bit) {
        value=value*value;
        if (bit>=5 || ((0x0bu>>bit)&1u)) value=value*input;
    }
    return value;
}
inline Bytes x25519(View scalar,View coordinate) {
    if (scalar.size!=32) fail("X25519 needs a 32-byte scalar");
    auto k=scalar.copy(); k[0]&=248; k[31]=(k[31]&127)|64;
    const auto x=Field25519::from(coordinate);
    Field25519 x2(1),z2(0),x3=x,z3(1); unsigned previous=0;
    for (int bit=254;bit>=0;--bit) {
        const unsigned current=(k[static_cast<unsigned>(bit)/8]>>(bit%8))&1u;
        swap(x2,x3,previous^current); swap(z2,z3,previous^current); previous=current;
        const auto a=x2+z2, aa=a*a, b=x2-z2, bb=b*b, e=aa-bb;
        const auto c=x3+z3,d=x3-z3,da=d*a,cb=c*b;
        x3=(da+cb)*(da+cb); z3=x*((da-cb)*(da-cb));
        x2=aa*bb; z2=e*(aa+Field25519(121665)*e);
    }
    swap(x2,x3,previous); swap(z2,z3,previous);
    auto result=(x2*inverse(z2)).bytes(); eraseSecret(k);
    eraseSecret(&x2,sizeof(x2)); eraseSecret(&z2,sizeof(z2)); eraseSecret(&x3,sizeof(x3)); eraseSecret(&z3,sizeof(z3));
    return result;
}
inline Bytes x25519Public(View secret) { Bytes base(32); base[0]=9; return x25519(secret,base); }
inline Bytes x25519Shared(View secret,View peer) {
    auto shared=x25519(secret,peer); unsigned combined=0;
    for (auto byte:shared) combined|=byte;
    if (!combined) { eraseSecret(shared); fail("X25519 peer has small order"); }
    return shared;
}

}}}  

namespace ncl { namespace detail { namespace tls {

enum class HashAlgorithm { Sha256, Sha384, Sha512 };
class Sha512 {
public:
    explicit Sha512(bool sha384=false) : sha384_(sha384) {
        state_=sha384 ? std::array<std::uint64_t,8>{{0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL}}
            : std::array<std::uint64_t,8>{{0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL}};
    }
    void update(View input) {
        if (input.size>UINT64_MAX/8-bytes_) fail("SHA-512 input exceeds bounds");
        bytes_+=input.size;
        std::size_t offset=0;
        while (offset<input.size) {
            const auto count=(std::min)(128-used_,input.size-offset);
            std::memcpy(buffer_.data()+used_,input.data+offset,count); used_+=count; offset+=count;
            if (used_==128) { block(buffer_.data()); used_=0; }
        }
    }
    Bytes finish() const {
        auto copy=*this; copy.buffer_[copy.used_++]=128;
        if (copy.used_>112) { std::fill(copy.buffer_.begin()+copy.used_,copy.buffer_.end(),Byte{0}); copy.block(copy.buffer_.data()); copy.used_=0; }
        std::fill(copy.buffer_.begin()+copy.used_,copy.buffer_.begin()+120,Byte{0}); put64(copy.buffer_.data()+120,copy.bytes_*8); copy.block(copy.buffer_.data());
        Bytes result(sha384_?48:64); for (std::size_t i=0;i<result.size()/8;++i) put64(result.data()+8*i,copy.state_[i]);
        eraseSecret(&copy,sizeof(copy)); return result;
    }
private:
    static std::uint64_t rotate(std::uint64_t x,unsigned n) noexcept { return (x>>n)|(x<<(64-n)); }
    void block(const Byte* input) {
        static constexpr std::uint64_t k[80]={
            0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
            0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
            0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
            0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
            0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
            0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
            0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
            0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
            0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
            0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};
        std::uint64_t w[80]; for (unsigned i=0;i<16;++i) w[i]=be64(input+8*i);
        for (unsigned i=16;i<80;++i) {
            const auto a=w[i-15],b=w[i-2]; w[i]=w[i-16]+(rotate(a,1)^rotate(a,8)^(a>>7))+w[i-7]+(rotate(b,19)^rotate(b,61)^(b>>6));
        }
        auto a=state_[0],b=state_[1],c=state_[2],d=state_[3],e=state_[4],f=state_[5],g=state_[6],h=state_[7];
        for (unsigned i=0;i<80;++i) {
            const auto first=h+(rotate(e,14)^rotate(e,18)^rotate(e,41))+((e&f)^(~e&g))+k[i]+w[i];
            const auto second=(rotate(a,28)^rotate(a,34)^rotate(a,39))+((a&b)^(a&c)^(b&c));
            h=g; g=f; f=e; e=d+first; d=c; c=b; b=a; a=first+second;
        }
        state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;
        eraseSecret(w,sizeof(w));
    }
    bool sha384_;
    std::array<std::uint64_t,8> state_{};
    std::array<Byte,128> buffer_{};
    std::size_t used_=0;
    std::uint64_t bytes_=0;
};
inline Bytes digest(HashAlgorithm algorithm,View input) {
    if (algorithm==HashAlgorithm::Sha256) return Sha256::hash(input);
    Sha512 hash(algorithm==HashAlgorithm::Sha384); hash.update(input); return hash.finish();
}

}}}

namespace ncl { namespace detail { namespace tls {

using Words=std::vector<std::uint32_t,ErasingAllocator<std::uint32_t>>;
inline void wipe(Words& value) noexcept { eraseSecret(value.data(),value.size()*sizeof(value[0])); }
inline Words words(View bytes,std::size_t count) {
    if (bytes.size>count*4) fail("oversized integer");
    Words value(count);
    for (std::size_t i=0;i<bytes.size;++i) value[i/4]|=std::uint32_t(bytes.data[bytes.size-1-i])<<(8*(i%4));
    return value;
}
inline Bytes bigEndian(const Words& value,std::size_t size=0) {
    if (!size) size=value.size()*4;
    Bytes result(size);
    for (std::size_t i=0;i<size && i/4<value.size();++i) result[size-1-i]=static_cast<Byte>(value[i/4]>>(8*(i%4)));
    return result;
}
inline std::uint32_t subtractWords(Words& result,const Words& a,const Words& b) {
    std::uint64_t borrow=0;
    for (std::size_t i=0;i<a.size();++i) {
        const auto difference=std::uint64_t(a[i])-b[i]-borrow;
        result[i]=static_cast<std::uint32_t>(difference); borrow=difference>>63;
    }
    return static_cast<std::uint32_t>(borrow);
}
inline bool less(const Words& a,const Words& b) { Words ignored(a.size()); return subtractWords(ignored,a,b)!=0; }
inline Words multiplyWords(const Words& a,const Words& b) {
    Words result(a.size()+b.size());
    for (std::size_t i=0;i<a.size();++i) {
        std::uint64_t carry=0;
        for (std::size_t j=0;j<b.size();++j) {
            const auto value=std::uint64_t(a[i])*b[j]+result[i+j]+carry;
            result[i+j]=static_cast<std::uint32_t>(value); carry=value>>32;
        }
        for (std::size_t j=i+b.size();j<result.size();++j) {
            const auto value=std::uint64_t(result[j])+carry;
            result[j]=static_cast<std::uint32_t>(value); carry=value>>32;
        }
    }
    return result;
}

 
 
class Montgomery {
public:
    explicit Montgomery(View modulus) : modulus_(words(modulus,(modulus.size+3)/4)), r2_(modulus_.size()), one_(modulus_.size()) {
        if (modulus.size==0 || modulus.size>512 || !(modulus_[0]&1u) || (modulus_.size()==1 && modulus_[0]<3))
            fail("invalid modular-arithmetic modulus");
        std::uint32_t inverse=1;
        for (unsigned i=0;i<5;++i) inverse*=2-modulus_[0]*inverse;
        inverse_=0u-inverse;
        r2_[0]=1;
        for (std::size_t i=0;i<64*modulus_.size();++i) doubleBit(r2_,0);
        Words plainOne(modulus_.size()); plainOne[0]=1; one_=multiply(plainOne,r2_);
    }
    ~Montgomery() { wipe(modulus_); wipe(r2_); wipe(one_); }
    Montgomery(const Montgomery&)=delete;
    Montgomery& operator=(const Montgomery&)=delete;
    std::size_t width() const noexcept { return modulus_.size(); }
    const Words& modulus() const noexcept { return modulus_; }
    Words reduce(View input) const {
        Words result(width());
        for (std::size_t i=0;i<input.size;++i) for (int bit=7;bit>=0;--bit) doubleBit(result,(input.data[i]>>bit)&1u);
        return result;
    }
    Words add(const Words& a,const Words& b) const {
        Words out(width()); std::uint64_t carry=0;
        for (std::size_t i=0;i<width();++i) {
            const auto value=std::uint64_t(a[i])+b[i]+carry; out[i]=static_cast<std::uint32_t>(value); carry=value>>32;
        }
        conditionalReduce(out,static_cast<std::uint32_t>(carry)); return out;
    }
    Words subtract(const Words& a,const Words& b) const {
        Words out(width()); const auto mask=0u-subtractWords(out,a,b); std::uint64_t carry=0;
        for (std::size_t i=0;i<width();++i) {
            const auto value=std::uint64_t(out[i])+(modulus_[i]&mask)+carry;
            out[i]=static_cast<std::uint32_t>(value); carry=value>>32;
        }
        return out;
    }
    Words multiply(const Words& a,const Words& b) const {
        const auto n=width(); Words work(2*n+2);
        for (std::size_t i=0;i<n;++i) {
            std::uint64_t carry=0;
            for (std::size_t j=0;j<n;++j) {
                const auto value=std::uint64_t(a[i])*b[j]+work[i+j]+carry;
                work[i+j]=static_cast<std::uint32_t>(value); carry=value>>32;
            }
            for (std::size_t j=i+n;j<work.size();++j) {
                const auto value=std::uint64_t(work[j])+carry; work[j]=static_cast<std::uint32_t>(value); carry=value>>32;
            }
        }
        for (std::size_t i=0;i<n;++i) {
            const std::uint32_t factor=work[i]*inverse_; std::uint64_t carry=0;
            for (std::size_t j=0;j<n;++j) {
                const auto value=std::uint64_t(factor)*modulus_[j]+work[i+j]+carry;
                work[i+j]=static_cast<std::uint32_t>(value); carry=value>>32;
            }
            for (std::size_t j=i+n;j<work.size();++j) {
                const auto value=std::uint64_t(work[j])+carry; work[j]=static_cast<std::uint32_t>(value); carry=value>>32;
            }
        }
        Words result(n); std::copy_n(work.begin()+n,n,result.begin());
        conditionalReduce(result,work[2*n]); wipe(work); return result;
    }
    Words in(const Words& normal) const { return multiply(normal,r2_); }
    Words out(const Words& montgomery) const {
        Words plainOne(width()); plainOne[0]=1; return multiply(montgomery,plainOne);
    }
    Words power(const Words& base,View exponent,std::size_t bits,bool secret,WaitContext* wait=nullptr) const {
        auto result=one_; auto factor=in(base);
        for (std::size_t remaining=bits;remaining;--remaining) {
            if (wait && (remaining%32==0)) wait->check();
            const std::size_t bit=remaining-1;
            const unsigned selected=bit/8<exponent.size ? ((exponent.data[exponent.size-1-bit/8]>>(bit%8))&1u) : 0;
            auto square=multiply(result,result); wipe(result); result=std::move(square);
            if (secret || selected) {
                auto product=multiply(result,factor); const auto mask=0u-selected;
                for (std::size_t i=0;i<width();++i) result[i]=(result[i]&~mask)|(product[i]&mask);
                wipe(product);
            }
        }
        auto normal=out(result); wipe(result); wipe(factor); return normal;
    }
private:
    void conditionalReduce(Words& value,std::uint32_t high) const {
        Words reduced(width()); const auto borrow=subtractWords(reduced,value,modulus_);
        const auto mask=0u-static_cast<std::uint32_t>((high!=0)|(borrow==0));
        for (std::size_t i=0;i<width();++i) value[i]=(value[i]&~mask)|(reduced[i]&mask);
        wipe(reduced);
    }
    void doubleBit(Words& value,unsigned bit) const {
        std::uint64_t carry=bit;
        for (std::size_t i=0;i<width();++i) {
            const auto next=std::uint64_t(value[i])*2+carry; value[i]=static_cast<std::uint32_t>(next); carry=next>>32;
        }
        conditionalReduce(value,static_cast<std::uint32_t>(carry));
    }
    Words modulus_,r2_,one_;
    std::uint32_t inverse_=0;
};

struct RsaPublic { Bytes n,e; };
struct RsaPrivate {
    RsaPublic publicKey;
    Bytes p,q,dp,dq,qi;
    RsaPrivate()=default;
    RsaPrivate(const RsaPrivate&)=delete;
    RsaPrivate& operator=(const RsaPrivate&)=delete;
    RsaPrivate(RsaPrivate&&)=default;
    RsaPrivate& operator=(RsaPrivate&&)=default;
    ~RsaPrivate() { eraseSecret(p); eraseSecret(q); eraseSecret(dp); eraseSecret(dq); eraseSecret(qi); }
};
inline std::size_t bitLength(View value) {
    std::size_t offset=0;
    while (offset<value.size && value.data[offset]==0) ++offset;
    if (offset==value.size) return 0;
    unsigned leading=0; for (Byte b=value.data[offset];!(b&128);b=static_cast<Byte>(b<<1)) ++leading;
    return (value.size-offset)*8-leading;
}
inline void validateRsa(const RsaPublic& key) {
    const auto bits=bitLength(key.n);
    if (bits<2048 || bits>4096 || key.n.size()>512 || key.n.empty() || !(key.n.back()&1u) ||
        key.e.empty() || key.e.size()>4 || !(key.e.back()&1u) || (key.e.size()==1 && key.e[0]<3))
        fail("RSA keys must use a 2048..4096-bit odd modulus and a small odd public exponent",HttpErrorKind::Certificate);
}
inline Bytes rsaPublic(const RsaPublic& key,View signature,WaitContext* wait=nullptr) {
    validateRsa(key);
    if (signature.size!=key.n.size()) return {};
    Montgomery context(key.n); auto encoded=words(signature,context.width());
    if (!less(encoded,context.modulus())) return {};
    auto decoded=context.power(encoded,key.e,key.e.size()*8,false,wait);
    return bigEndian(decoded,key.n.size());
}
inline Bytes mgf1(View seed,std::size_t length,HashAlgorithm algorithm=HashAlgorithm::Sha256) {
    Bytes out;
    for (std::uint32_t i=0;out.size()<length;++i) {
        Bytes input=seed.copy(); integer(input,i,4); auto hashed=digest(algorithm,input);
        append(out,View(hashed).sub(0,(std::min)(hashed.size(),length-out.size())));
    }
    return out;
}
inline Bytes pkcs1Encoding(View digest,std::size_t size) {
    Byte prefix[]={0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
    if ((digest.size!=32 && digest.size!=48 && digest.size!=64) || size<sizeof(prefix)+digest.size+11) fail("invalid RSA digest size");
    prefix[1]=static_cast<Byte>(17+digest.size); prefix[14]=digest.size==32?1:digest.size==48?2:3; prefix[18]=static_cast<Byte>(digest.size);
    Bytes out(size,0xff); out[0]=0; out[1]=1;
    const auto separator=size-sizeof(prefix)-digest.size-1; out[separator]=0;
    std::copy(std::begin(prefix),std::end(prefix),out.begin()+separator+1);
    std::copy(digest.data,digest.data+digest.size,out.end()-digest.size); return out;
}
inline Bytes pssEncoding(View digest,std::size_t bits) {
    const auto size=(bits+7)/8;
    if (digest.size!=32 || size<66) fail("invalid RSA-PSS digest size");
    auto salt=random(32); Bytes input(8); append(input,digest); append(input,salt); auto hash=Sha256::hash(input);
    Bytes db(size-33); db[db.size()-33]=1; std::copy(salt.begin(),salt.end(),db.end()-32);
    auto mask=mgf1(hash,db.size());
    for (std::size_t i=0;i<db.size();++i) db[i]^=mask[i];
    db[0]&=static_cast<Byte>(255u>>(8*size-bits)); append(db,hash); db.push_back(0xbc);
    eraseSecret(salt); return db;
}
inline bool verifyPss(View hashed,View encoded,std::size_t bits,HashAlgorithm algorithm=HashAlgorithm::Sha256) {
    const auto size=(bits+7)/8;
    const auto hashSize=hashed.size;
    if ((hashSize!=32 && hashSize!=48 && hashSize!=64) || size<2*hashSize+2 || encoded.size<size) return false;
    for (std::size_t i=0;i<encoded.size-size;++i) if (encoded.data[i]) return false;
    encoded=encoded.sub(encoded.size-size,size);
    if (encoded.data[size-1]!=0xbc || (encoded.data[0] & (255u<<(8-(8*size-bits))))) return false;
    const auto hash=encoded.sub(size-hashSize-1,hashSize); auto db=encoded.sub(0,size-hashSize-1).copy(); const auto mask=mgf1(hash,db.size(),algorithm);
    for (std::size_t i=0;i<db.size();++i) db[i]^=mask[i];
    db[0]&=static_cast<Byte>(255u>>(8*size-bits));
    unsigned bad=0; for (std::size_t i=0;i<db.size()-hashSize-1;++i) bad|=db[i];
    bad|=db[db.size()-hashSize-1]^1u;
    Bytes input(8); append(input,hashed); append(input,View(db).sub(db.size()-hashSize,hashSize));
    return bad==0 && equal(digest(algorithm,input),hash);
}
inline bool verifyRsa(const RsaPublic& key,View message,View signature,bool pss,WaitContext* wait=nullptr,HashAlgorithm algorithm=HashAlgorithm::Sha256) {
    const auto encoded=rsaPublic(key,signature,wait); if (encoded.empty()) return false;
    const auto hashed=digest(algorithm,message);
    return pss ? verifyPss(hashed,encoded,bitLength(key.n)-1,algorithm) : equal(encoded,pkcs1Encoding(hashed,key.n.size()));
}
inline Bytes blindExponent(View exponent,const Montgomery& context) {
    auto minusOne=context.modulus();
    for (std::size_t i=0;i<minusOne.size();++i) { if (minusOne[i]--) break; }
    auto entropy=random(8); const auto multiplier=words(entropy,2);
    auto product=multiplyWords(minusOne,multiplier); const auto base=words(exponent,context.width());
    std::uint64_t carry=0;
    for (std::size_t i=0;i<product.size();++i) {
        const auto value=std::uint64_t(product[i])+(i<base.size()?base[i]:0)+carry;
        product[i]=static_cast<std::uint32_t>(value); carry=value>>32;
    }
    auto result=bigEndian(product); wipe(product); wipe(minusOne); eraseSecret(entropy); return result;
}
inline Bytes signRsa(const RsaPrivate& key,View message,bool pss,WaitContext* wait=nullptr) {
    validateRsa(key.publicKey);
    const auto digest=Sha256::hash(message);
    auto encoded=pss ? pssEncoding(digest,bitLength(key.publicKey.n)-1) : pkcs1Encoding(digest,key.publicKey.n.size());
    Montgomery p(key.p),q(key.q);
    auto ep=blindExponent(key.dp,p),eq=blindExponent(key.dq,q);
    auto m1=p.power(p.reduce(encoded),ep,ep.size()*8,true,wait);
    auto m2=q.power(q.reduce(encoded),eq,eq.size()*8,true,wait);
    auto m2p=p.reduce(bigEndian(m2)); auto difference=p.subtract(m1,m2p);
    auto h=p.out(p.multiply(p.in(difference),p.in(p.reduce(key.qi))));
    auto result=multiplyWords(words(key.q,q.width()),h);
    result.resize((key.publicKey.n.size()+3)/4);
    std::uint64_t carry=0;
    for (std::size_t i=0;i<result.size();++i) {
        const auto sum=std::uint64_t(result[i])+(i<m2.size()?m2[i]:0)+carry;
        result[i]=static_cast<std::uint32_t>(sum); carry=sum>>32;
    }
    auto signature=bigEndian(result,key.publicKey.n.size());
    wipe(result); wipe(m1); wipe(m2); wipe(m2p); wipe(difference); wipe(h); eraseSecret(ep); eraseSecret(eq); eraseSecret(encoded);
     
    if (!verifyRsa(key.publicKey,message,signature,pss,wait)) { eraseSecret(signature); fail("RSA private-key operation failed its verification"); }
    return signature;
}

}}}

namespace ncl { namespace detail { namespace tls {

struct DerNode { Byte tag=0; View content,whole; };
class Der {
public:
    explicit Der(View value) : view_(value) {}
    bool empty() const noexcept { return position_==view_.size; }
    Byte peek() const { if (empty()) fail("truncated DER"); return view_.data[position_]; }
    DerNode node(int expected=-1) {
        const auto start=position_;
        if (view_.size-position_<2) fail("truncated DER object");
        const Byte tag=view_.data[position_++];
        if ((tag&31)==31 || (expected>=0 && tag!=expected)) fail("unexpected DER tag");
        std::size_t length=view_.data[position_++];
        if (length&128) {
            const auto count=length&127;
            if (!count || count>4 || count>view_.size-position_ || view_.data[position_]==0) fail("invalid DER length");
            length=0;
            for (std::size_t i=0;i<count;++i) length=(length<<8)|view_.data[position_++];
            if (length<128) fail("non-minimal DER length");
        }
        if (length>view_.size-position_) fail("truncated DER contents");
        const auto content=view_.sub(position_,length); position_+=length;
        return {tag,content,view_.sub(start,position_-start)};
    }
    View value(int tag) { return node(tag).content; }
    void end() const { if (!empty()) fail("trailing DER objects"); }
private:
    View view_;
    std::size_t position_=0;
};
inline bool oid(View value,std::initializer_list<Byte> expected) {
    return equal(value,View(expected.begin(),expected.size()));
}
inline Bytes positiveInteger(Der& reader) {
    auto value=reader.value(2);
    if (!value.size || (value.data[0]&128)) fail("negative or empty DER integer");
    if (value.size>1 && value.data[0]==0) {
        if (!(value.data[1]&128)) fail("non-minimal DER integer");
        value=value.sub(1,value.size-1);
    }
    return value.copy();
}
inline bool derBoolean(View value) {
    if (value.size!=1 || (value.data[0]!=0 && value.data[0]!=255)) fail("invalid DER boolean");
    return value.data[0]!=0;
}
inline View bitString(Der& reader) {
    const auto value=reader.value(3);
    if (!value.size || value.data[0]!=0) fail("unaligned DER bit string");
    return value.sub(1,value.size-1);
}
inline Bytes readFile(const std::string& filename,std::size_t cap=16*1024*1024) {
    if (filename.empty() || filename.find('\0')!=std::string::npos) fail("invalid certificate/key filename",HttpErrorKind::InvalidArgument);
    std::ifstream stream(filename,std::ios::binary);
    if (!stream) fail("cannot open certificate/key file: "+filename);
    Bytes result; std::array<char,4096> buffer{};
    while (stream) {
        stream.read(buffer.data(),buffer.size()); const auto count=static_cast<std::size_t>(stream.gcount());
        if (count>cap-result.size()) fail("certificate/key file exceeds limit");
        append(result,View(reinterpret_cast<const Byte*>(buffer.data()),count));
    }
    if (!stream.eof()) fail("cannot read certificate/key file");
    return result;
}
inline Bytes base64(std::string_view text) {
    Bytes compact;
    for (unsigned char c:text) {
        if (c==' ' || c=='\t' || c=='\r' || c=='\n') continue;
        compact.push_back(c);
    }
    if (compact.size()%4) fail("invalid PEM base64 length");
    const auto decode=[](Byte c)->unsigned {
        if (c>='A' && c<='Z') return c-'A';
        if (c>='a' && c<='z') return c-'a'+26;
        if (c>='0' && c<='9') return c-'0'+52;
        if (c=='+') return 62;
        if (c=='/') return 63;
        fail("invalid PEM base64 character");
    };
    Bytes result;
    for (std::size_t offset=0;offset<compact.size();offset+=4) {
        const auto a=decode(compact[offset]),b=decode(compact[offset+1]);
        const bool pad2=compact[offset+2]=='=',pad3=compact[offset+3]=='=';
        if ((pad2 || pad3) && offset+4!=compact.size()) fail("PEM padding before the end");
        if (pad2 && !pad3) fail("invalid PEM padding");
        const auto c=pad2?0:decode(compact[offset+2]),d=pad3?0:decode(compact[offset+3]);
        if ((pad2 && (b&15)) || (pad3 && !pad2 && (c&3))) fail("non-canonical PEM padding");
        result.push_back(static_cast<Byte>((a<<2)|(b>>4)));
        if (!pad2) result.push_back(static_cast<Byte>((b<<4)|(c>>2)));
        if (!pad3) result.push_back(static_cast<Byte>((c<<6)|d));
    }
    eraseSecret(compact); return result;
}
inline std::vector<Bytes> pem(View file,std::string_view labelName,std::size_t maxObjects=1024) {
    if (!file.size) return {};
    const std::basic_string<char,std::char_traits<char>,ErasingAllocator<char>> text(reinterpret_cast<const char*>(file.data),file.size);
    const std::string begin="-----BEGIN "+std::string(labelName)+"-----";
    const std::string end="-----END "+std::string(labelName)+"-----";
    std::vector<Bytes> result; std::size_t position=0;
    for (;;) {
        const auto start=text.find(begin.c_str(),position); if (start==std::string::npos) break;
        const auto finish=text.find(end.c_str(),start+begin.size()); if (finish==std::string::npos) fail("unterminated PEM object");
        if (result.size()>=maxObjects) fail("too many PEM objects");
        result.push_back(base64(std::string_view(text.data(),text.size()).substr(start+begin.size(),finish-start-begin.size())));
        position=finish+end.size();
    }
    return result;
}
inline RsaPublic rsaSpki(View encoded) {
    Der outer(encoded); Der root(outer.value(0x30)); outer.end();
    Der algorithm(root.value(0x30)); const auto name=algorithm.value(6);
    if (!oid(name,{0x2a,0x86,0x48,0x86,0xf7,0x0d,1,1,1})) fail("unsupported certificate public-key algorithm",HttpErrorKind::Certificate);
    if (!algorithm.empty()) { if (algorithm.value(5).size) fail("invalid RSA parameters"); } algorithm.end();
    const auto bits=bitString(root); root.end(); Der publicBits(bits); Der values(publicBits.value(0x30)); publicBits.end();
    RsaPublic key; key.n=positiveInteger(values); key.e=positiveInteger(values); values.end(); validateRsa(key); return key;
}
inline RsaPrivate rsaPrivate(const std::string& filename) {
    auto file=readFile(filename,65536); auto objects=pem(file,"RSA PRIVATE KEY",1);
    if (objects.empty()) {
        objects=pem(file,"PRIVATE KEY",1);
        if (objects.empty()) { eraseSecret(file); fail("an unencrypted RSA PRIVATE KEY or PRIVATE KEY PEM is required"); }
        Der outer(objects[0]); Der root(outer.value(0x30)); outer.end();
        const auto version=positiveInteger(root);
        if (version.size()!=1 || version[0]!=0) fail("unsupported PKCS#8 version");
        Der algorithm(root.value(0x30)); const auto name=algorithm.value(6);
        if (!oid(name,{0x2a,0x86,0x48,0x86,0xf7,0x0d,1,1,1})) fail("only RSA server private keys are supported");
        if (!algorithm.empty()) { if (algorithm.value(5).size) fail("invalid private-key parameters"); } algorithm.end();
        auto inner=root.value(4).copy(); root.end(); eraseSecret(objects[0]); objects[0]=std::move(inner);
    }
    eraseSecret(file); Der outer(objects[0]); Der values(outer.value(0x30)); outer.end();
    const auto version=positiveInteger(values);
    if (version.size()!=1 || version[0]!=0) fail("multi-prime RSA is unsupported");
    RsaPrivate key; key.publicKey.n=positiveInteger(values); key.publicKey.e=positiveInteger(values);
    auto d=positiveInteger(values); eraseSecret(d);
    key.p=positiveInteger(values); key.q=positiveInteger(values);
    key.dp=positiveInteger(values); key.dq=positiveInteger(values); key.qi=positiveInteger(values); values.end();
    eraseSecret(objects[0]); validateRsa(key.publicKey);
    if (bitLength(key.p)<1024 || bitLength(key.q)<1024 || key.p.size()>256 || key.q.size()>256 ||
        !(key.p.back()&1u) || !(key.q.back()&1u)) fail("invalid RSA private primes");
    const auto product=multiplyWords(words(key.p,(key.p.size()+3)/4),words(key.q,(key.q.size()+3)/4));
    if (!equal(bigEndian(product,key.publicKey.n.size()),key.publicKey.n)) fail("RSA prime product does not match modulus");
    Montgomery p(key.p),q(key.q);
    if (!bitLength(key.dp) || !bitLength(key.dq) || !bitLength(key.qi) ||
        key.dp.size()>key.p.size() || key.dq.size()>key.q.size() || key.qi.size()>key.p.size() ||
        !less(words(key.dp,p.width()),p.modulus()) || !less(words(key.dq,q.width()),q.modulus()) || !less(words(key.qi,p.width()),p.modulus()))
        fail("invalid RSA CRT parameters");
    return key;
}

}}}

namespace ncl { namespace detail { namespace tls {

inline Bytes hexBytes(std::string_view text) {
    if (text.size()%2) fail("invalid hexadecimal constant");
    Bytes result;
    for (std::size_t i=0;i<text.size();i+=2) {
        const auto a=hexDigit(text[i]),b=hexDigit(text[i+1]); if (a<0 || b<0) fail("invalid hexadecimal constant");
        result.push_back(static_cast<Byte>(16*a+b));
    }
    return result;
}
 
 
class EcCurve {
    struct Point { Words x,y,z; };
public:
    explicit EcCurve(bool p384=false) : bytes_(p384?48:32),width_(bytes_/4),bits_(bytes_*8),
        prime_(hexBytes(p384?"fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff":"ffffffff00000001000000000000000000000000ffffffffffffffffffffffff")),
        order_(hexBytes(p384?"ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973":"ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551")),
        generator_(hexBytes(p384?"04aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab73617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f":"046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2964fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5")),
        field_(prime_),scalar_(order_),b_(field_.in(words(hexBytes(p384?"b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef":"5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b"),width_))) {}
    void validate(View publicPoint) const { (void)point(publicPoint); }
    bool verify(View publicPoint,View message,View signature,WaitContext* wait=nullptr,HashAlgorithm algorithm=HashAlgorithm::Sha256) const {
        Der outer(signature); Der pair(outer.value(0x30)); outer.end(); auto rBytes=positiveInteger(pair),sBytes=positiveInteger(pair); pair.end();
        if (!bitLength(rBytes) || !bitLength(sBytes) || rBytes.size()>bytes_ || sBytes.size()>bytes_) return false;
        const auto r=words(rBytes,width_),s=words(sBytes,width_);
        if (!less(r,scalar_.modulus()) || !less(s,scalar_.modulus())) return false;
        auto inverseExponent=order_; inverseExponent.at(bytes_-1)-=2;
        const auto inverse=scalar_.power(s,inverseExponent,bits_,false,wait);
        auto hashed=digest(algorithm,message); if (hashed.size()>bytes_) hashed.resize(bytes_);
        const auto z=scalar_.reduce(hashed);
        const auto u1=scalar_.out(scalar_.multiply(scalar_.in(z),scalar_.in(inverse)));
        const auto u2=scalar_.out(scalar_.multiply(scalar_.in(r),scalar_.in(inverse)));
        const auto generator=point(generator_);
        const auto result=add(multiply(generator,u1,wait),multiply(point(publicPoint),u2,wait));
        if (infinity(result)) return false;
        auto primeExponent=prime_; primeExponent.at(bytes_-1)-=2;
        const auto inverseZ=field_.in(field_.power(field_.out(result.z),primeExponent,bits_,false,wait));
        const auto x=field_.out(field_.multiply(result.x,field_.multiply(inverseZ,inverseZ)));
        return scalar_.reduce(bigEndian(x,bytes_))==r;
    }
    Bytes secret() const {
        for (unsigned attempt=0;attempt<128;++attempt) {
            auto value=random(bytes_);
            if (bitLength(value) && less(words(value,width_),scalar_.modulus())) return value;
            eraseSecret(value);
        }
        fail("cannot sample an EC scalar");
    }
    Bytes publicKey(View secret,WaitContext* wait=nullptr) const {
        const auto result=secretMultiply(point(generator_),privateScalar(secret),wait); const auto pair=affine(result,wait);
        Bytes encoded{4}; append(encoded,bigEndian(pair.first,bytes_)); append(encoded,bigEndian(pair.second,bytes_)); return encoded;
    }
    Bytes shared(View secret,View peer,WaitContext* wait=nullptr) const {
        const auto result=secretMultiply(point(peer),privateScalar(secret),wait);
        if (infinity(result)) fail("invalid ECDH result");
        return bigEndian(affine(result,wait).first,bytes_);
    }
private:
    static bool zero(const Words& value) { std::uint32_t combined=0; for (auto word:value) combined|=word; return combined==0; }
    bool infinity(const Point& point) const { return zero(point.z); }
    Point empty() const { return {Words(width_),Words(width_),Words(width_)}; }
    Words multiplySmall(const Words& value,unsigned factor) const {
        auto result=Words(width_),addend=value;
        while (factor) { if (factor&1u) result=field_.add(result,addend); addend=field_.add(addend,addend); factor>>=1; }
        return result;
    }
    Point point(View encoded) const {
        if (encoded.size!=1+2*bytes_ || encoded.data[0]!=4) fail("EC requires an uncompressed public point",HttpErrorKind::Certificate);
        const auto x=words(encoded.sub(1,bytes_),width_),y=words(encoded.sub(1+bytes_,bytes_),width_);
        if (!less(x,field_.modulus()) || !less(y,field_.modulus())) fail("P-256 coordinate is outside the field",HttpErrorKind::Certificate);
        const auto xm=field_.in(x),ym=field_.in(y);
        const auto right=field_.subtract(field_.add(field_.multiply(field_.multiply(xm,xm),xm),b_),multiplySmall(xm,3));
        if (field_.multiply(ym,ym)!=right) fail("public key is not on P-256",HttpErrorKind::Certificate);
        Words one(width_);one[0]=1; return {xm,ym,field_.in(one)};
    }
    Point twice(const Point& p) const {
        if (infinity(p) || zero(p.y)) return empty();
        return twiceRaw(p);
    }
    Point twiceRaw(const Point& p) const {
        const auto delta=field_.multiply(p.z,p.z),gamma=field_.multiply(p.y,p.y),beta=field_.multiply(p.x,gamma);
        const auto alpha=multiplySmall(field_.multiply(field_.subtract(p.x,delta),field_.add(p.x,delta)),3);
        const auto x=field_.subtract(field_.multiply(alpha,alpha),multiplySmall(beta,8));
        const auto y=field_.subtract(field_.multiply(alpha,field_.subtract(multiplySmall(beta,4),x)),multiplySmall(field_.multiply(gamma,gamma),8));
        const auto yz=field_.add(p.y,p.z);
        const auto z=field_.subtract(field_.subtract(field_.multiply(yz,yz),gamma),delta);
        return {x,y,z};
    }
    Point addRaw(const Point& a,const Point& b) const {
        const auto za=field_.multiply(a.z,a.z),zb=field_.multiply(b.z,b.z);
        const auto u1=field_.multiply(a.x,zb),u2=field_.multiply(b.x,za);
        const auto s1=field_.multiply(a.y,field_.multiply(b.z,zb)),s2=field_.multiply(b.y,field_.multiply(a.z,za));
        const auto h=field_.subtract(u2,u1),twoH=multiplySmall(h,2),i=field_.multiply(twoH,twoH),j=field_.multiply(h,i);
        const auto r=multiplySmall(field_.subtract(s2,s1),2),v=field_.multiply(u1,i);
        const auto x=field_.subtract(field_.subtract(field_.multiply(r,r),j),multiplySmall(v,2));
        const auto y=field_.subtract(field_.multiply(r,field_.subtract(v,x)),multiplySmall(field_.multiply(s1,j),2));
        const auto zz=field_.add(a.z,b.z);
        const auto z=field_.multiply(field_.subtract(field_.subtract(field_.multiply(zz,zz),za),zb),h);
        Point result{x,y,z},coincident=empty();
        select(coincident,twiceRaw(a),equalMask(s1,s2)); select(result,coincident,equalMask(u1,u2));
        select(result,b,zeroMask(a.z)); select(result,a,zeroMask(b.z)); return result;
    }
    Point add(const Point& a,const Point& b) const {
        return addRaw(a,b);
    }
    Point multiply(const Point& point,const Words& scalar,WaitContext* wait) const {
        auto result=empty();
        for (std::size_t remaining=bits_;remaining;--remaining) {
            if (wait && remaining%32==0) wait->check();
            result=twice(result); const auto bit=remaining-1;
            if ((scalar[bit/32]>>(bit%32))&1u) result=add(result,point);
        }
        return result;
    }
    static std::uint32_t zeroMask(const Words& value) {
        std::uint32_t bits=0; for (auto word:value) bits|=word;
        return 0u-(1u-((bits|(0u-bits))>>31));
    }
    static std::uint32_t equalMask(const Words& a,const Words& b) {
        Words difference(a.size()); for (std::size_t i=0;i<a.size();++i) difference[i]=a[i]^b[i]; return zeroMask(difference);
    }
    void select(Point& result,const Point& alternative,std::uint32_t mask) const {
        for (std::size_t i=0;i<width_;++i) {
            result.x[i]=(result.x[i]&~mask)|(alternative.x[i]&mask);
            result.y[i]=(result.y[i]&~mask)|(alternative.y[i]&mask);
            result.z[i]=(result.z[i]&~mask)|(alternative.z[i]&mask);
        }
    }
    Words privateScalar(View secret) const {
        if (secret.size!=bytes_) fail("EC private scalar has an invalid size");
        auto value=words(secret,width_);
        if (zero(value) || !less(value,scalar_.modulus())) fail("EC private scalar is out of range");
        return value;
    }
    Point secretMultiply(const Point& point,const Words& scalar,WaitContext* wait) const {
        auto result=empty();
        for (std::size_t remaining=bits_;remaining;--remaining) {
            if (wait && remaining%16==0) wait->check();
            result=twiceRaw(result); const auto sum=addRaw(result,point); const auto bit=remaining-1;
            select(result,sum,0u-((scalar[bit/32]>>(bit%32))&1u));
        }
        return result;
    }
    std::pair<Words,Words> affine(const Point& point,WaitContext* wait) const {
        auto exponent=prime_; exponent.at(bytes_-1)-=2;
        const auto inverseZ=field_.in(field_.power(field_.out(point.z),exponent,bits_,false,wait));
        const auto square=field_.multiply(inverseZ,inverseZ);
        return {field_.out(field_.multiply(point.x,square)),field_.out(field_.multiply(point.y,field_.multiply(square,inverseZ)))};
    }
    std::size_t bytes_,width_,bits_;
    Bytes prime_,order_,generator_;
    Montgomery field_,scalar_;
    Words b_;
};
inline Bytes ecPublicSpki(View encoded) {
    Der outer(encoded); Der root(outer.value(0x30)); outer.end(); Der algorithm(root.value(0x30));
    if (!oid(algorithm.value(6),{0x2a,0x86,0x48,0xce,0x3d,2,1})) fail("invalid EC public-key algorithm");
    const auto curve=algorithm.value(6); const bool p384=oid(curve,{0x2b,0x81,4,0,0x22});
    if (!p384 && !oid(curve,{0x2a,0x86,0x48,0xce,0x3d,3,1,7})) fail("only P-256/P-384 ECDSA public keys are supported",HttpErrorKind::Certificate);
    algorithm.end(); auto point=bitString(root).copy(); root.end(); EcCurve(p384).validate(point); return point;
}
inline bool ecSpki(View encoded) {
    Der outer(encoded); Der root(outer.value(0x30)); Der algorithm(root.value(0x30));
    return oid(algorithm.value(6),{0x2a,0x86,0x48,0xce,0x3d,2,1});
}
inline bool verifyEcdsa(View publicPoint,View message,View signature,WaitContext* wait=nullptr,HashAlgorithm algorithm=HashAlgorithm::Sha256) {
    try { return EcCurve(publicPoint.size==97).verify(publicPoint,message,signature,wait,algorithm); }
    catch (const HttpError&) { return false; }
}

}}}

namespace ncl { namespace detail { namespace tls {

inline std::int64_t certificateTime(const DerNode& node) {
    const auto text=node.content;
    const unsigned digits=node.tag==0x17?12:node.tag==0x18?14:0;
    if (!digits || text.size!=digits+1 || text.data[digits]!='Z') fail("certificate time must use UTC seconds",HttpErrorKind::Certificate);
    const auto decimal=[&](unsigned offset,unsigned count) {
        unsigned n=0;
        for (unsigned i=0;i<count;++i) { const auto c=text.data[offset+i]; if (c<'0' || c>'9') fail("invalid certificate date"); n=n*10+c-'0'; }
        return n;
    };
    unsigned year=decimal(0,digits==12?2:4),offset=digits==12?2:4;
    if (digits==12) year+=year>=50?1900:2000;
    const auto month=decimal(offset,2),day=decimal(offset+2,2),hour=decimal(offset+4,2),minute=decimal(offset+6,2),second=decimal(offset+8,2);
    static constexpr unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap=year%4==0 && (year%100!=0 || year%400==0);
    if (year<1601 || year>9999 || month<1 || month>12 || day<1 || day>days[month-1]+(month==2 && leap) || hour>23 || minute>59 || second>59)
        fail("out-of-range certificate date",HttpErrorKind::Certificate);
    const auto y=static_cast<std::int64_t>(year)-(month<=2); const auto era=y/400; const auto yoe=y-era*400;
    const auto adjusted=static_cast<std::int64_t>(month)+(month>2?-3:9);
    const auto doy=(153*adjusted+2)/5+day-1; const auto doe=yoe*365+yoe/4-yoe/100+doy;
    return (era*146097+doe-719468)*86400+hour*3600+minute*60+second;
}
enum class SignatureAlgorithm { Unsupported, RsaSha256, RsaSha384, RsaSha512, RsaPssSha256, EcdsaSha256, EcdsaSha384, EcdsaSha512 };
inline bool sha256Identifier(View encoded) {
    Der root(encoded); Der algorithm(root.value(0x30)); root.end();
    if (!oid(algorithm.value(6),{0x60,0x86,0x48,1,0x65,3,4,2,1})) return false;
    if (!algorithm.empty() && algorithm.value(5).size) return false;
    algorithm.end(); return true;
}
inline SignatureAlgorithm signatureAlgorithm(View encoded) {
    Der root(encoded); Der algorithm(root.value(0x30)); root.end(); const auto name=algorithm.value(6);
    if (oid(name,{0x2a,0x86,0x48,0x86,0xf7,0x0d,1,1,11}) || oid(name,{0x2a,0x86,0x48,0x86,0xf7,0x0d,1,1,12}) || oid(name,{0x2a,0x86,0x48,0x86,0xf7,0x0d,1,1,13})) {
        if (!algorithm.empty() && algorithm.value(5).size) fail("invalid RSA signature parameters");
        algorithm.end(); return name.data[name.size-1]==11?SignatureAlgorithm::RsaSha256:name.data[name.size-1]==12?SignatureAlgorithm::RsaSha384:SignatureAlgorithm::RsaSha512;
    }
    if (oid(name,{0x2a,0x86,0x48,0xce,0x3d,4,3,2})) { algorithm.end(); return SignatureAlgorithm::EcdsaSha256; }
    if (oid(name,{0x2a,0x86,0x48,0xce,0x3d,4,3,3})) { algorithm.end(); return SignatureAlgorithm::EcdsaSha384; }
    if (oid(name,{0x2a,0x86,0x48,0xce,0x3d,4,3,4})) { algorithm.end(); return SignatureAlgorithm::EcdsaSha512; }
    if (oid(name,{0x2a,0x86,0x48,0x86,0xf7,0x0d,1,1,10})) {
        if (algorithm.empty()) return SignatureAlgorithm::Unsupported;
        Der parameters(algorithm.value(0x30)); algorithm.end();
        bool hash=false,mask=false,salt=false;
        if (!parameters.empty() && parameters.peek()==0xa0) hash=sha256Identifier(parameters.value(0xa0));
        if (!parameters.empty() && parameters.peek()==0xa1) {
            Der explicitMask(parameters.value(0xa1)); Der maskAlgorithm(explicitMask.value(0x30)); explicitMask.end();
            if (oid(maskAlgorithm.value(6),{0x2a,0x86,0x48,0x86,0xf7,0x0d,1,1,8})) mask=sha256Identifier(maskAlgorithm.node(0x30).whole);
            maskAlgorithm.end();
        }
        if (!parameters.empty() && parameters.peek()==0xa2) {
            Der length(parameters.value(0xa2)); const auto value=positiveInteger(length); length.end(); salt=value.size()==1 && value[0]==32;
        }
        if (!parameters.empty() && parameters.peek()==0xa3) {
            Der trailer(parameters.value(0xa3)); const auto value=positiveInteger(trailer); trailer.end();
            if (value.size()!=1 || value[0]!=1) return SignatureAlgorithm::Unsupported;
        }
        parameters.end(); return hash && mask && salt?SignatureAlgorithm::RsaPssSha256:SignatureAlgorithm::Unsupported;
    }
    return SignatureAlgorithm::Unsupported;
}

struct Certificate {
    Bytes der,tbs,issuer,subject,spki,signature;
    RsaPublic key;
    Bytes ecPoint;
    SignatureAlgorithm algorithm=SignatureAlgorithm::Unsupported;
    std::int64_t notBefore=0,notAfter=0;
    bool ca=false,hasUsage=false,hasEku=false,serverAuth=false;
    unsigned usage=0;
    std::optional<std::size_t> pathLength;
    std::vector<std::string> dns;
    std::vector<Bytes> ips;
};
inline void certificateExtensions(Certificate& certificate,View encoded) {
    Der explicitExtensions(encoded); Der extensions(explicitExtensions.value(0x30)); explicitExtensions.end();
    std::vector<Bytes> seen;
    while (!extensions.empty()) {
        if (seen.size()>=128) fail("too many certificate extensions");
        Der extension(extensions.value(0x30)); const auto name=extension.value(6);
        for (const auto& previous:seen) if (equal(name,previous)) fail("duplicate certificate extension");
        seen.push_back(name.copy()); bool critical=false;
        if (!extension.empty() && extension.peek()==1) critical=derBoolean(extension.value(1));
        const auto contents=extension.value(4); extension.end();
        if (oid(name,{0x55,0x1d,0x13})) {
            Der outer(contents); Der value(outer.value(0x30)); outer.end();
            if (!value.empty() && value.peek()==1) certificate.ca=derBoolean(value.value(1));
            if (!value.empty()) {
                const auto length=positiveInteger(value);
                if (!certificate.ca || length.size()>4) fail("invalid CA path-length constraint");
                std::size_t count=0; for (auto byte:length) count=(count<<8)|byte; certificate.pathLength=count;
            }
            value.end();
        } else if (oid(name,{0x55,0x1d,0x0f})) {
            Der outer(contents); const auto bits=outer.value(3); outer.end();
            if (bits.size<2 || bits.size>3 || bits.data[0]>7 || (bits.data[bits.size-1]&((1u<<bits.data[0])-1))) fail("invalid certificate key usage");
            certificate.hasUsage=true; certificate.usage=unsigned(bits.data[1])<<8;
            if (bits.size==3) certificate.usage|=bits.data[2];
        } else if (oid(name,{0x55,0x1d,0x25})) {
            Der outer(contents); Der usages(outer.value(0x30)); outer.end(); certificate.hasEku=true;
            if (usages.empty()) fail("empty extended key usage");
            while (!usages.empty()) {
                const auto usage=usages.value(6);
                if (oid(usage,{0x2b,6,1,5,5,7,3,1}) || oid(usage,{0x55,0x1d,0x25,0})) certificate.serverAuth=true;
            }
        } else if (oid(name,{0x55,0x1d,0x11})) {
            Der outer(contents); Der names(outer.value(0x30)); outer.end();
            if (names.empty()) fail("empty subjectAltName");
            while (!names.empty()) {
                const auto entry=names.node();
                if (entry.tag==0x82) {
                    if (!entry.content.size || entry.content.size>253) fail("invalid DNS subjectAltName");
                    std::string dns;
                    for (std::size_t i=0;i<entry.content.size;++i) {
                        const auto c=entry.content.data[i];
                        if (!((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='.' || c=='-' || c=='*')) fail("invalid DNS subjectAltName");
                        dns+=asciiLower(static_cast<char>(c));
                    }
                    certificate.dns.push_back(std::move(dns));
                } else if (entry.tag==0x87) {
                    if (entry.content.size!=4 && entry.content.size!=16) fail("invalid IP subjectAltName");
                    certificate.ips.push_back(entry.content.copy());
                }
            }
        } else if (oid(name,{0x55,0x1d,0x1e}) || oid(name,{0x55,0x1d,0x24}) || oid(name,{0x55,0x1d,0x21}) || oid(name,{0x55,0x1d,0x36})) {
             
             
            fail("certificate path constraints are outside the supported profile",HttpErrorKind::Certificate);
        } else if (critical) {
            fail("unsupported critical certificate extension",HttpErrorKind::Certificate);
        }
    }
}
inline Certificate parseCertificate(View encoded) {
    try {
        if (encoded.size>128*1024) fail("certificate exceeds 128 KiB");
        Certificate certificate; certificate.der=encoded.copy();
        Der outer(certificate.der); Der root(outer.value(0x30)); outer.end();
        const auto tbs=root.node(0x30); certificate.tbs=tbs.whole.copy();
        const auto algorithm=root.node(0x30); certificate.algorithm=signatureAlgorithm(algorithm.whole);
        certificate.signature=bitString(root).copy(); root.end(); Der body(tbs.content);
        unsigned versionNumber=0;
        if (body.peek()==0xa0) {
            Der explicitVersion(body.value(0xa0)); const auto version=positiveInteger(explicitVersion); explicitVersion.end();
            if (version.size()!=1 || version[0]>2) fail("unsupported X.509 version");
            versionNumber=version[0];
        }
        const auto serial=positiveInteger(body); if (serial.empty() || serial.size()>20 || !bitLength(serial)) fail("invalid X.509 serial number");
        if (!equal(body.node(0x30).whole,algorithm.whole)) fail("inconsistent certificate signature algorithms");
        certificate.issuer=body.node(0x30).whole.copy();
        Der validity(body.value(0x30)); certificate.notBefore=certificateTime(validity.node()); certificate.notAfter=certificateTime(validity.node()); validity.end();
        if (certificate.notAfter<certificate.notBefore) fail("inverted certificate validity");
        certificate.subject=body.node(0x30).whole.copy(); certificate.spki=body.node(0x30).whole.copy();
        if (ecSpki(certificate.spki)) certificate.ecPoint=ecPublicSpki(certificate.spki); else certificate.key=rsaSpki(certificate.spki);
        if (!body.empty() && versionNumber==0) fail("X.509 v1 certificate contains later-version fields");
        if (!body.empty() && body.peek()==0x81) (void)body.node(0x81);
        if (!body.empty() && body.peek()==0x82) (void)body.node(0x82);
        if (!body.empty()) { if (versionNumber!=2) fail("X.509 extensions require v3"); certificateExtensions(certificate,body.value(0xa3)); }
        body.end(); return certificate;
    } catch (const HttpError& error) {
        throw HttpError(HttpErrorKind::Certificate,error.what());
    }
}
inline void certificateValidity(const Certificate& certificate,std::int64_t now) {
    if (now<certificate.notBefore || now>certificate.notAfter) fail("certificate is not currently valid",HttpErrorKind::Certificate);
    if (certificate.hasEku && !certificate.serverAuth) fail("certificate does not permit server authentication",HttpErrorKind::Certificate);
}
inline void certificateIdentity(const Certificate& certificate,const std::string& host,bool numeric) {
    if (numeric) {
        std::array<Byte,16> address{}; const int family=host.find(':')==std::string::npos?AF_INET:AF_INET6;
        if (::inet_pton(family,host.c_str(),address.data())!=1) fail("invalid numeric certificate identity",HttpErrorKind::Certificate);
        for (const auto& ip:certificate.ips) if (equal(ip,View(address.data(),family==AF_INET?4:16))) return;
    } else {
        std::string name=host; for (auto& c:name) c=asciiLower(c);
        if (!name.empty() && name.back()=='.') name.pop_back();
        for (const auto& dns:certificate.dns) {
            if (dns==name) return;
            if (dns.size()>2 && dns[0]=='*' && dns[1]=='.' && dns.find('*',1)==std::string::npos && dns.find('.',2)!=std::string::npos && name.size()>dns.size()-1) {
                const auto prefix=name.size()-(dns.size()-1);
                if (name.compare(prefix,std::string::npos,dns,1,std::string::npos)==0 && name.substr(0,prefix).find('.')==std::string::npos && name.rfind("xn--",0)!=0) return;
            }
        }
    }
    fail("certificate subjectAltName does not match the requested host",HttpErrorKind::Certificate);
}
inline bool certificateSignature(const Certificate& certificate,const Certificate& issuer,WaitContext& wait) {
    if (certificate.algorithm==SignatureAlgorithm::Unsupported) return false;
    if (certificate.algorithm==SignatureAlgorithm::EcdsaSha256 || certificate.algorithm==SignatureAlgorithm::EcdsaSha384 || certificate.algorithm==SignatureAlgorithm::EcdsaSha512) {
        const auto hash=certificate.algorithm==SignatureAlgorithm::EcdsaSha256?HashAlgorithm::Sha256:certificate.algorithm==SignatureAlgorithm::EcdsaSha384?HashAlgorithm::Sha384:HashAlgorithm::Sha512;
        return !issuer.ecPoint.empty() && verifyEcdsa(issuer.ecPoint,certificate.tbs,certificate.signature,&wait,hash);
    }
    if (!issuer.ecPoint.empty()) return false;
    const auto hash=certificate.algorithm==SignatureAlgorithm::RsaSha384?HashAlgorithm::Sha384:certificate.algorithm==SignatureAlgorithm::RsaSha512?HashAlgorithm::Sha512:HashAlgorithm::Sha256;
    return verifyRsa(issuer.key,certificate.tbs,certificate.signature,certificate.algorithm==SignatureAlgorithm::RsaPssSha256,&wait,hash);
}
inline std::vector<Certificate> parseAnchors(const std::vector<Bytes>& objects) {
    std::vector<Certificate> result;
    for (const auto& encoded:objects) {
        try { result.push_back(parseCertificate(encoded)); }
        catch (const HttpError&) {   }
    }
    if (result.empty()) fail("no supported trust anchors are available; configure caFile",HttpErrorKind::Certificate);
    return result;
}

#ifdef _WIN32
class SystemLibrary {
public:
    explicit SystemLibrary(const wchar_t* name) : module_(::LoadLibraryExW(name,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!module_) fail("cannot load a Windows system service");
    }
    ~SystemLibrary() { ::FreeLibrary(module_); }
    SystemLibrary(const SystemLibrary&)=delete;
    SystemLibrary& operator=(const SystemLibrary&)=delete;
    template<class Function> Function function(const char* name) const {
        const auto address=::GetProcAddress(module_,name); Function result=nullptr;
        static_assert(sizeof(result)==sizeof(address),"Windows function pointer size"); std::memcpy(&result,&address,sizeof(result));
        if (!result) fail("missing Windows system service");
        return result;
    }
private:
    HMODULE module_;
};
#endif
inline std::vector<Certificate> systemAnchors() {
    std::vector<Bytes> objects;
#ifdef _WIN32
    const SystemLibrary library(L"crypt32.dll");
    const auto open=library.function<HCERTSTORE (WINAPI*)(HCRYPTPROV_LEGACY,LPCWSTR)>("CertOpenSystemStoreW");
    const auto next=library.function<PCCERT_CONTEXT (WINAPI*)(HCERTSTORE,PCCERT_CONTEXT)>("CertEnumCertificatesInStore");
    const auto close=library.function<BOOL (WINAPI*)(HCERTSTORE,DWORD)>("CertCloseStore");
    const auto release=library.function<BOOL (WINAPI*)(PCCERT_CONTEXT)>("CertFreeCertificateContext");
    const auto store=open(0,L"ROOT");
    if (!store) fail("cannot open Windows ROOT certificate store",HttpErrorKind::Certificate);
    PCCERT_CONTEXT current=nullptr;
    try {
        while ((current=next(store,current))!=nullptr) {
            if (objects.size()>=4096 || current->cbCertEncoded>128*1024) fail("system trust store exceeds bounds");
            objects.emplace_back(current->pbCertEncoded,current->pbCertEncoded+current->cbCertEncoded);
        }
    } catch (...) { if (current) release(current); close(store,0); throw; }
    close(store,0);
#else
    for (const auto* name:{"/etc/ssl/certs/ca-certificates.crt","/etc/pki/tls/certs/ca-bundle.crt","/etc/ssl/ca-bundle.pem"}) {
        std::ifstream exists(name,std::ios::binary);
        if (exists) { objects=pem(readFile(name),"CERTIFICATE",4096); break; }
    }
#endif
    return parseAnchors(objects);
}
inline std::vector<Certificate> trustAnchors(const std::string& filename) {
    if (filename.empty()) { static const auto anchors=systemAnchors(); return anchors; }
    try { return parseAnchors(pem(readFile(filename),"CERTIFICATE",4096)); }
    catch (const HttpError& error) { throw HttpError(HttpErrorKind::Certificate,error.what()); }
}
inline bool sameAnchor(const Certificate& certificate,const Certificate& anchor) {
    return equal(certificate.subject,anchor.subject) && equal(certificate.spki,anchor.spki);
}
inline bool buildPath(const Certificate& current,const std::vector<Certificate>& chain,const std::vector<Certificate>& anchors,
    std::vector<Bytes>& visited,std::size_t caBelow,std::int64_t now,WaitContext& wait,std::string* reason=nullptr) {
    wait.check();
    certificateValidity(current,now);
    for (const auto& anchor:anchors) if (sameAnchor(current,anchor)) { certificateValidity(anchor,now); return true; }
    if (visited.size()>=8) return false;
    visited.push_back(current.der);
    const auto tryIssuer=[&](const Certificate& candidate) {
        if (!equal(current.issuer,candidate.subject)) return false;
        if (!candidate.ca || (candidate.hasUsage && !(candidate.usage&0x0400)) || (candidate.pathLength && caBelow>*candidate.pathLength)) {
            if (reason) *reason="issuer CA, signing usage or path-length constraint rejected";
            return false;
        }
        for (const auto& previous:visited) if (equal(previous,candidate.der)) return false;
        try {
            certificateValidity(candidate,now);
            if (!certificateSignature(current,candidate,wait)) {
                if (reason) *reason="issuer signature is invalid or uses an unsupported algorithm";
                return false;
            }
            return buildPath(candidate,chain,anchors,visited,caBelow+(!equal(candidate.subject,candidate.issuer)?1u:0u),now,wait,reason);
        } catch (const HttpError& error) { if (reason) *reason=error.what(); return false; }
    };
    for (const auto& candidate:chain) if (tryIssuer(candidate)) { visited.pop_back(); return true; }
    for (const auto& candidate:anchors) if (tryIssuer(candidate)) { visited.pop_back(); return true; }
    visited.pop_back(); return false;
}
inline void verifyCertificates(const std::vector<Certificate>& chain,const HttpUrl& url,const std::string& caFile,WaitContext& wait) {
    if (chain.empty()) fail("server did not provide a certificate",HttpErrorKind::Certificate);
    const auto& leaf=chain.front();
    if (leaf.hasUsage && !(leaf.usage&0x8000)) fail("server certificate does not permit digital signatures",HttpErrorKind::Certificate);
    certificateIdentity(leaf,url.host,url.numeric);
    const auto now=std::time(nullptr); if (now<0) fail("system clock cannot validate certificates",HttpErrorKind::Certificate);
    const auto anchors=trustAnchors(caFile); std::vector<Bytes> visited; std::string reason="no matching supported issuer";
    if (!buildPath(leaf,chain,anchors,visited,0,static_cast<std::int64_t>(now),wait,&reason)) fail("certificate chain does not reach a supported trusted authority: "+reason,HttpErrorKind::Certificate);
}
struct ServerIdentity { std::vector<Certificate> chain; RsaPrivate key; };
inline ServerIdentity serverIdentity(const std::string& certificateFile,const std::string& keyFile) {
    ServerIdentity identity;
    for (const auto& encoded:pem(readFile(certificateFile),"CERTIFICATE",8)) identity.chain.push_back(parseCertificate(encoded));
    if (identity.chain.empty()) fail("server certificate file contains no certificates");
    if (!identity.chain[0].ecPoint.empty()) fail("server identities currently require an RSA certificate and key");
    identity.key=rsaPrivate(keyFile);
    if (!equal(identity.chain[0].key.n,identity.key.publicKey.n) || !equal(identity.chain[0].key.e,identity.key.publicKey.e))
        fail("server certificate and private key do not match");
    return identity;
}

}}}

namespace ncl { namespace detail { namespace tls {

class Records {
public:
    using Write=std::function<void(const void*,std::size_t,WaitContext&)>;
    using Read=std::function<std::size_t(void*,std::size_t,WaitContext&)>;
    Records(Write write,Read read) : write_(std::move(write)),read_(std::move(read)) {}
    Records(const Records&)=delete;
    Records& operator=(const Records&)=delete;
protected:
    struct ClosingTransportEnd {};
    struct Traffic {
        Bytes secret,key,iv;
        std::uint64_t sequence=0;
        bool enabled=false;
        ~Traffic() { eraseSecret(secret); eraseSecret(key); eraseSecret(iv); }
    };
    struct Record { Byte type=0; Bytes body; };
    void traffic(Traffic& state,View secret) {
        eraseSecret(state.secret); eraseSecret(state.key); eraseSecret(state.iv);
        state.secret=secret.copy(); state.key=label(secret,"key",{},16); state.iv=label(secret,"iv",{},12);
        state.sequence=0; state.enabled=true;
    }
    Bytes nonce(const Traffic& state) const {
        Bytes result=state.iv;
        for (unsigned i=0;i<8;++i) result[result.size()-1-i]^=static_cast<Byte>(state.sequence>>(8*i));
        return result;
    }
    void exact(void* buffer,std::size_t size,WaitContext& wait) {
        auto* output=static_cast<Byte*>(buffer); std::size_t done=0;
        while (done<size) {
            wait.check();
            if (inputPosition_==inputSize_) {
                inputSize_=read_(input_.data(),input_.size(),wait); inputPosition_=0;
                if (!inputSize_) {
                    if (closing_) throw ClosingTransportEnd{};
                    fail("connection ended without an authenticated close notification");
                }
            }
            const auto count=(std::min)(size-done,inputSize_-inputPosition_);
            std::memcpy(output+done,input_.data()+inputPosition_,count); inputPosition_+=count; done+=count;
        }
    }
    bool bufferedRecord() const noexcept {
        const auto available=inputSize_-inputPosition_;
        return available>=5 && available>=5+(std::size_t(input_[inputPosition_+3])<<8)+input_[inputPosition_+4];
    }
    void sendRecord(Byte type,View content,WaitContext& wait,bool forcePlain=false) {
        wait.check();
        if (content.size>16384) fail("outgoing TLS plaintext is too long");
        Bytes body=content.copy(); Byte outerType=type;
        if (outgoing_.enabled && !forcePlain) {
            if (outgoing_.sequence>=(std::uint64_t{1}<<24)) fail("traffic key reached its record limit");
            if (tls13_) {
                body.push_back(type); outerType=23;
                Bytes aad{outerType,3,3}; integer(aad,body.size()+16,2);
                auto ciphertext=gcm(outgoing_.key,nonce(outgoing_),aad,body,false); eraseSecret(body); body=std::move(ciphertext);
            } else {
                auto explicitNonce=number(outgoing_.sequence,8); Bytes iv=outgoing_.iv; append(iv,explicitNonce);
                Bytes aad=number(outgoing_.sequence,8); aad.push_back(type); integer(aad,0x0303,2); integer(aad,content.size,2);
                auto ciphertext=gcm(outgoing_.key,iv,aad,content,false); body=std::move(explicitNonce); append(body,ciphertext);
            }
            ++outgoing_.sequence;
        }
        Bytes header{outerType,3,3}; integer(header,body.size(),2);
        append(header,body); write_(header.data(),header.size(),wait);
    }
    Record readRecord(WaitContext& wait) {
        for (;;) {
            wait.check(); std::array<Byte,5> header{}; exact(header.data(),header.size(),wait);
            const auto size=(unsigned(header[3])<<8)|header[4];
            if (header[1]!=3 || header[2]<1 || header[2]>3 || size>16384+256) fail("invalid TLS record header");
            Bytes body(size); exact(body.data(),body.size(),wait); Byte type=header[0];
            if (type==20 && tls13_ && !complete_ && allowCompatibility_) {
                if (++compatibilityRecords_>8 || body!=Bytes{1}) fail("invalid compatibility ChangeCipherSpec");
                continue;
            }
            if (incoming_.enabled) {
                if (header[2]!=3 || incoming_.sequence>=(std::uint64_t{1}<<24)) fail("invalid encrypted record version or exhausted traffic key");
                if (tls13_) {
                    if (type!=23) fail("unencrypted record after TLS 1.3 key establishment");
                    body=gcm(incoming_.key,nonce(incoming_),header,body,true);
                    while (!body.empty() && body.back()==0) body.pop_back();
                    if (body.empty()) fail("TLS 1.3 record has no content type");
                    type=body.back(); body.pop_back();
                    if (type!=21 && type!=22 && type!=23) fail("invalid TLS 1.3 inner content type");
                } else {
                    if (body.size()<24) fail("truncated TLS 1.2 AEAD record");
                    Bytes iv=incoming_.iv; append(iv,View(body).sub(0,8));
                    Bytes aad=number(incoming_.sequence,8); aad.push_back(type); integer(aad,0x0303,2); integer(aad,body.size()-24,2);
                    body=gcm(incoming_.key,iv,aad,View(body).sub(8,body.size()-8),true);
                }
                ++incoming_.sequence;
            }
            if (body.size()>16384) fail("TLS plaintext record exceeds 16 KiB");
            if (type==21) {
                if (body.size()!=2 || (body[0]!=1 && body[0]!=2)) fail("invalid TLS alert");
                if (body[1]==0 && incoming_.enabled && complete_) { receivedClose_=true; return {21,std::move(body)}; }
                fail("peer sent TLS alert "+std::to_string(body[1]));
            }
            return {type,std::move(body)};
        }
    }
    void sendHandshake(Byte type,View contents,WaitContext& wait,bool transcript=true) {
        Bytes message{type}; integer(message,contents.size,3); append(message,contents);
        if (transcript) remember(message);
        for (std::size_t offset=0;offset<message.size();offset+=16384)
            sendRecord(22,View(message).sub(offset,(std::min)(std::size_t{16384},message.size()-offset)),wait);
    }
    Bytes receiveHandshake(WaitContext& wait) {
        unsigned empty=0;
        for (;;) {
            if (pending_.size()>=4) {
                const auto size=(std::size_t(pending_[1])<<16)|(std::size_t(pending_[2])<<8)|pending_[3];
                if (size>1024*1024-4) fail("TLS handshake message exceeds 1 MiB");
                if (pending_.size()>=size+4) {
                    Bytes message(pending_.begin(),pending_.begin()+size+4); pending_.erase(pending_.begin(),pending_.begin()+size+4); return message;
                }
            }
            auto record=readRecord(wait);
            if (record.type!=22) fail("unexpected record during TLS handshake");
            if (record.body.empty() && ++empty>32) fail("too many empty handshake records");
            if (record.body.size()>1024*1024-pending_.size()) fail("TLS handshake buffering exceeds 1 MiB");
            append(pending_,record.body);
        }
    }
    static View handshakeBody(const Bytes& message,Byte expected) {
        if (message.size()<4 || message[0]!=expected) fail("unexpected TLS handshake message");
        return View(message).sub(4,message.size()-4);
    }
    void remember(View message) {
        if (message.size>1024*1024-transcript_.size()) fail("TLS handshake transcript exceeds 1 MiB");
        append(transcript_,message);
    }
    void changeCipherSpec(WaitContext& wait,bool writing) {
        if (writing) { sendRecord(20,Bytes{1},wait,true); outgoing_.enabled=true; outgoing_.sequence=0; }
        else {
            if (!pending_.empty()) fail("handshake fragments cross ChangeCipherSpec");
            const auto record=readRecord(wait);
            if (record.type!=20 || record.body!=Bytes{1}) fail("expected TLS 1.2 ChangeCipherSpec");
            incoming_.enabled=true; incoming_.sequence=0;
        }
    }
    void alert(Byte level,Byte description,WaitContext& wait) { sendRecord(21,Bytes{level,description},wait); }

    Write write_;
    Read read_;
    Traffic incoming_,outgoing_;
    bool tls13_=true,complete_=false,receivedClose_=false,sentClose_=false,allowCompatibility_=false,closing_=false;
    unsigned compatibilityRecords_=0;
    Bytes transcript_,pending_;
    std::array<Byte,32768> input_{};
    std::size_t inputPosition_=0,inputSize_=0;
};

using Extensions=std::vector<std::pair<unsigned,Bytes>>;
inline Extensions extensions(View encoded) {
    Reader reader(encoded); Extensions result;
    while (reader.remaining()) {
        if (result.size()>=256) fail("too many TLS extensions");
        const auto type=static_cast<unsigned>(reader.uint(2)); const auto body=reader.vec(2);
        for (const auto& entry:result) if (entry.first==type) fail("duplicate TLS extension");
        result.emplace_back(type,body.copy());
    }
    return result;
}
inline const Bytes* extension(const Extensions& list,unsigned type) {
    for (const auto& item:list) if (item.first==type) return &item.second;
    return nullptr;
}
inline void addExtension(Bytes& list,unsigned type,View value) { integer(list,type,2); append(list,vector(value,2)); }
inline bool contains16(View encoded,unsigned value) {
    Reader reader(encoded); bool found=false;
    while (reader.remaining()) if (reader.uint(2)==value) found=true;
    return found;
}
inline Bytes clientHello(View randomValue,View session,View publicKey,const HttpUrl& url,View cookie={},unsigned keyGroup=0x1d) {
    Bytes hello=number(0x0303,2); append(hello,randomValue); append(hello,vector(session,1));
    append(hello,Bytes{0,6,0x13,1,0xc0,0x2f,0xc0,0x2b,1,0});
    Bytes ext;
    if (!url.numeric) {
        Bytes name{0}; append(name,vector(View(url.host),2)); addExtension(ext,0,vector(name,2));
    }
    addExtension(ext,10,Bytes{0,6,0,0x1d,0,0x17,0,0x18});
    addExtension(ext,11,Bytes{1,0});
    addExtension(ext,13,Bytes{0,8,8,4,4,1,4,3,5,3});
    addExtension(ext,50,Bytes{0,12,4,1,5,1,6,1,4,3,5,3,6,3});
    addExtension(ext,43,Bytes{4,3,4,3,3});
    Bytes share=number(keyGroup,2); append(share,vector(publicKey,2)); addExtension(ext,51,vector(share,2));
    Bytes protocol=vector(View("http/1.1"),1); addExtension(ext,16,vector(protocol,2));
    addExtension(ext,23,{}); addExtension(ext,0xff01,Bytes{0});
    if (cookie.size) addExtension(ext,44,vector(cookie,2));
    append(hello,vector(ext,2)); return hello;
}
struct ClientHello {
    Bytes random,session,ciphers,publicKey,p256Key,p384Key,signatureAlgorithms,groups;
    bool version13=false,version12=true,ems=false,alpn=false,sni=false;
    Extensions extensions;
};
inline ClientHello parseClientHello(View body) {
    Reader reader(body); ClientHello hello;
    if (reader.uint(2)!=0x0303) fail("only TLS 1.2 and TLS 1.3 are supported");
    hello.random=reader.take(32).copy(); hello.session=reader.vec(1).copy();
    if (hello.session.size()>32) fail("oversized TLS session ID");
    hello.ciphers=reader.vec(2).copy();
    if (hello.ciphers.empty() || hello.ciphers.size()%2) fail("invalid cipher-suite list");
    const auto compression=reader.vec(1);
    if (compression.size!=1 || compression.data[0]!=0) fail("TLS compression is unsupported");
    if (reader.remaining()) hello.extensions=extensions(reader.vec(2));
    reader.end();
    if (const auto* supported=extension(hello.extensions,43)) {
        Reader versions(*supported); const auto list=versions.vec(1); versions.end();
        hello.version13=contains16(list,0x0304); hello.version12=contains16(list,0x0303);
    }
    if (const auto* groups=extension(hello.extensions,10)) { Reader r(*groups); hello.groups=r.vec(2).copy(); r.end(); }
    if (const auto* algorithms=extension(hello.extensions,13)) { Reader r(*algorithms); hello.signatureAlgorithms=r.vec(2).copy(); r.end(); }
    if (const auto* shares=extension(hello.extensions,51)) {
        Reader outer(*shares); Reader r(outer.vec(2)); outer.end(); std::vector<unsigned> seen;
        while (r.remaining()) {
            const auto group=static_cast<unsigned>(r.uint(2)); const auto key=r.vec(2);
            if (std::find(seen.begin(),seen.end(),group)!=seen.end()) fail("duplicate key share");
            seen.push_back(group);
            if (!contains16(hello.groups,group)) fail("key share was not listed in supported groups");
            if (group==0x1d) { if (key.size!=32) fail("invalid X25519 key share"); hello.publicKey=key.copy(); }
            if (group==0x17) { if (key.size!=65 || key.data[0]!=4) fail("invalid P-256 key share"); hello.p256Key=key.copy(); }
            if (group==0x18) { if (key.size!=97 || key.data[0]!=4) fail("invalid P-384 key share"); hello.p384Key=key.copy(); }
        }
    }
    if (const auto* ems=extension(hello.extensions,23)) { if (!ems->empty()) fail("invalid extended-master-secret extension"); hello.ems=true; }
    if (const auto* alpn=extension(hello.extensions,16)) {
        Reader outer(*alpn); Reader protocols(outer.vec(2)); outer.end(); bool found=false;
        while (protocols.remaining()) { const auto name=protocols.vec(1); if (!name.size) fail("empty ALPN name"); if (equal(name,View("http/1.1"))) found=true; }
        if (!found) fail("no common ALPN protocol");
        hello.alpn=true;
    }
    if (const auto* names=extension(hello.extensions,0)) {
        Reader outer(*names); Reader list(outer.vec(2)); outer.end(); bool found=false;
        while (list.remaining()) {
            const auto type=list.uint(1); const auto name=list.vec(2);
            if (type!=0 || found || !name.size || name.size>253) fail("invalid SNI server name");
            for (std::size_t i=0;i<name.size;++i) if (name.data[i]<33 || name.data[i]>126 || name.data[i]==0) fail("invalid SNI server name");
            found=true;
        }
        if (!found) fail("empty SNI extension");
        hello.sni=true;
    }
    if (const auto* renegotiate=extension(hello.extensions,0xff01)) if (*renegotiate!=Bytes{0}) fail("TLS renegotiation is unsupported");
    if (extension(hello.extensions,42)) fail("TLS early data is unsupported");
    return hello;
}
inline const Bytes& clientShare(const ClientHello& hello,unsigned group) {
    if (group==0x1d) return hello.publicKey;
    if (group==0x17) return hello.p256Key;
    return hello.p384Key;
}
inline std::size_t groupKeyLength(unsigned group) {
    if (group==0x1d) return 32;
    if (group==0x17) return 65;
    if (group==0x18) return 97;
    fail("unsupported ECDHE group");
}

}}}

namespace ncl { namespace detail { namespace tls {

struct ServerHello {
    Bytes random,session,publicKey;
    unsigned cipher=0;
    unsigned group=0;
    bool version13=false,ems=false;
    Extensions extensions;
};
inline ServerHello parseServerHello(View body,View clientSession,bool numeric,unsigned offeredGroup=0x1d) {
    Reader reader(body); ServerHello hello;
    if (reader.uint(2)!=0x0303) fail("server selected an unsupported TLS version");
    hello.random=reader.take(32).copy(); hello.session=reader.vec(1).copy();
    if (hello.session.size()>32) fail("invalid server session ID");
    hello.cipher=static_cast<unsigned>(reader.uint(2));
    if (reader.uint(1)!=0) fail("server selected TLS compression");
    if (reader.remaining()) hello.extensions=extensions(reader.vec(2));
    reader.end();
    if (const auto* version=extension(hello.extensions,43)) {
        if (*version!=Bytes{3,4}) fail("server selected an unoffered TLS version");
        hello.version13=true;
    }
    if (hello.version13) {
        if (hello.cipher!=0x1301 || !equal(hello.session,clientSession)) fail("invalid TLS 1.3 negotiation");
        for (const auto& ext:hello.extensions) if (ext.first!=43 && ext.first!=51) fail("unsolicited ServerHello extension");
        const auto* share=extension(hello.extensions,51); if (!share) fail("server omitted X25519 key share");
        Reader key(*share); hello.group=static_cast<unsigned>(key.uint(2));
        if (hello.group!=offeredGroup) fail("server selected a key share absent from ClientHello");
        hello.publicKey=key.vec(2).copy(); key.end();
        if (hello.publicKey.size()!=groupKeyLength(hello.group) || (hello.group!=0x1d && hello.publicKey[0]!=4)) fail("invalid server key share");
    } else {
        if (hello.cipher!=0xc02f && hello.cipher!=0xc02b) fail("server selected an unsupported TLS 1.2 cipher suite");
        const auto marker=View(hello.random).sub(24,8);
        static constexpr Byte downgrade[]={0x44,0x4f,0x57,0x4e,0x47,0x52,0x44,1};
        if (equal(marker,View(downgrade,8))) fail("TLS downgrade sentinel detected");
        for (const auto& ext:hello.extensions) {
            if (ext.first==23) { if (!ext.second.empty()) fail("invalid extended-master-secret response"); hello.ems=true; }
            else if (ext.first==0xff01) { if (ext.second!=Bytes{0}) fail("server attempted renegotiation"); }
            else if (ext.first==0) { if (numeric || !ext.second.empty()) fail("invalid SNI acknowledgment"); }
            else if (ext.first==16) {
                Reader alpn(ext.second); Reader names(alpn.vec(2)); alpn.end();
                if (!equal(names.vec(1),View("http/1.1"))) fail("server selected an unsupported ALPN protocol");
                names.end();
            } else if (ext.first==11) {
                Reader formats(ext.second); const auto list=formats.vec(1); formats.end();
                if (!list.size || std::find(list.data,list.data+list.size,Byte{0})==list.data+list.size) fail("unsupported EC point format");
            } else fail("unsolicited TLS 1.2 ServerHello extension");
        }
        if (!hello.ems) fail("TLS 1.2 requires the extended-master-secret extension");
    }
    return hello;
}
inline Bytes verifyContext(View transcript) {
    Bytes input(64,32); append(input,View("TLS 1.3, server CertificateVerify")); input.push_back(0); append(input,Sha256::hash(transcript)); return input;
}
inline std::vector<Certificate> peerCertificates(View body,bool version13) {
    Reader reader(body);
    if (version13 && reader.vec(1).size) fail("unexpected certificate request context");
    Reader list(reader.vec(3)); reader.end(); std::vector<Certificate> chain;
    while (list.remaining()) {
        if (chain.size()>=8) fail("certificate chain exceeds eight entries",HttpErrorKind::Certificate);
        chain.push_back(parseCertificate(list.vec(3)));
        if (version13 && list.vec(2).size) fail("unsolicited certificate-entry extensions");
    }
    if (chain.empty()) fail("empty server certificate chain",HttpErrorKind::Certificate);
    return chain;
}
inline Bytes certificateMessage(const ServerIdentity& identity,bool version13) {
    Bytes list;
    for (const auto& certificate:identity.chain) { append(list,vector(certificate.der,3)); if (version13) integer(list,0,2); }
    Bytes message; if (version13) message.push_back(0); append(message,vector(list,3)); return message;
}
inline Bytes retryRandom() { return hexBytes("cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c"); }
inline bool isRetry(View body) { return body.size>=34 && equal(body.sub(2,32),retryRandom()); }
struct RetryRequest { unsigned group=0x1d; Bytes cookie; };
inline RetryRequest retryRequest(View body,View session) {
    Reader reader(body);
    if (reader.uint(2)!=0x0303 || !equal(reader.take(32),retryRandom()) || !equal(reader.vec(1),session) || reader.uint(2)!=0x1301 || reader.uint(1)!=0)
        fail("invalid HelloRetryRequest");
    const auto ext=extensions(reader.vec(2)); reader.end();
    const auto* version=extension(ext,43);
    if (!version || *version!=Bytes{3,4}) fail("HelloRetryRequest omitted TLS 1.3 version");
    const auto* cookie=extension(ext,44);
    for (const auto& entry:ext) if (entry.first!=43 && entry.first!=44 && entry.first!=51) fail("unsolicited HelloRetryRequest extension");
    RetryRequest result; bool changed=false;
    if (const auto* group=extension(ext,51)) {
        Reader selected(*group); const auto name=selected.uint(2); selected.end();
        if (name==0x1d) fail("HelloRetryRequest repeated the already supplied X25519 key share");
        if (name!=0x17 && name!=0x18) fail("HelloRetryRequest selected an unsupported key-exchange group");
        result.group=static_cast<unsigned>(name); changed=true;
    }
    if (cookie) {
        Reader encoded(*cookie); result.cookie=encoded.vec(2).copy(); encoded.end(); if (result.cookie.empty()) fail("empty TLS retry cookie"); changed=true;
    }
    if (!changed) fail("HelloRetryRequest did not change ClientHello");
    return result;
}
inline Bytes groupPublic(unsigned group,View secret,WaitContext& wait) {
    wait.check(); auto result=group==0x1d?x25519Public(secret):EcCurve(group==0x18).publicKey(secret,&wait); wait.check(); return result;
}
inline Bytes groupShared(unsigned group,View secret,View peer,WaitContext& wait) {
    wait.check();
    try { auto result=group==0x1d?x25519Shared(secret,peer):EcCurve(group==0x18).shared(secret,peer,&wait); wait.check(); return result; }
    catch (const HttpError&) { fail("invalid ECDHE peer key"); }
}

class Connection : private Records {
public:
    Connection(Write write,Read read) : Records(std::move(write),std::move(read)) {}
    ~Connection() { eraseSecret(privateKey_); eraseSecret(handshakeSecret_); eraseSecret(master_); eraseSecret(clientHandshake_); eraseSecret(serverHandshake_); }
    void client(const HttpUrl& url,const HttpOptions& options,WaitContext& wait) {
        try {
            clientRandom_=random(32); session_=random(32); privateKey_=random(32);
            sendHandshake(1,clientHello(clientRandom_,session_,x25519Public(privateKey_),url),wait);
            allowCompatibility_=true;
            auto message=receiveHandshake(wait);
            if (isRetry(handshakeBody(message,2))) {
                const auto retry=retryRequest(handshakeBody(message,2),session_); retryTranscript(); remember(message);
                group_=retry.group;
                if (group_!=0x1d) { eraseSecret(privateKey_); privateKey_=EcCurve(group_==0x18).secret(); }
                sendHandshake(1,clientHello(clientRandom_,session_,groupPublic(group_,privateKey_,wait),url,retry.cookie,group_),wait);
                message=receiveHandshake(wait);
                if (isRetry(handshakeBody(message,2))) fail("server sent a second HelloRetryRequest");
            }
            const auto hello=parseServerHello(handshakeBody(message,2),session_,url.numeric,group_);
            serverRandom_=hello.random; tls13_=hello.version13; cipher_=hello.cipher; remember(message);
            if (tls13_) client13(hello,url,options,wait); else client12(url,options,wait);
            handshakeComplete();
        } catch (const HttpError&) { abort(wait); throw; }
    }
    void server(const ServerIdentity& identity,WaitContext& wait) {
        try {
            serverSide_=true;
            auto message=receiveHandshake(wait); auto hello=parseClientHello(handshakeBody(message,1)); remember(message);
            allowCompatibility_=true;
            clientRandom_=hello.random; session_=hello.session; serverRandom_=random(32); privateKey_=random(32);
            tls13_=hello.version13 && contains16(hello.ciphers,0x1301);
            if (tls13_) {
                if (!hello.publicKey.empty()) group_=0x1d;
                else if (!hello.p256Key.empty()) group_=0x17;
                else if (!hello.p384Key.empty()) group_=0x18;
                else group_=contains16(hello.groups,0x1d)?0x1d:contains16(hello.groups,0x17)?0x17:contains16(hello.groups,0x18)?0x18:0;
                if (!group_) fail("no common ECDHE group");
                if (hello.publicKey.empty() && hello.p256Key.empty() && hello.p384Key.empty()) {
                    if (extension(hello.extensions,41)) fail("HelloRetryRequest with a PSK offer is outside the supported profile");
                    Bytes retry=number(0x0303,2); append(retry,retryRandom()); append(retry,vector(session_,1)); integer(retry,0x1301,2); retry.push_back(0);
                    Bytes ext; addExtension(ext,43,Bytes{3,4}); addExtension(ext,51,number(group_,2)); append(retry,vector(ext,2));
                    retryTranscript(); sendHandshake(2,retry,wait); message=receiveHandshake(wait); auto updated=parseClientHello(handshakeBody(message,1));
                    const auto comparable=[](const Extensions& list) { Extensions result; for (const auto& entry:list) if (entry.first!=51 && entry.first!=21) result.push_back(entry); return result; };
                    if (updated.random!=hello.random || updated.session!=hello.session || updated.ciphers!=hello.ciphers || comparable(updated.extensions)!=comparable(hello.extensions) || clientShare(updated,group_).size()!=groupKeyLength(group_))
                        fail("ClientHello changed forbidden fields after HelloRetryRequest");
                    hello=std::move(updated); remember(message);
                }
                if (group_!=0x1d) { eraseSecret(privateKey_); privateKey_=EcCurve(group_==0x18).secret(); }
                server13(hello,identity,wait);
            }
            else {
                if (!hello.version12 || !contains16(hello.ciphers,0xc02f)) fail("no supported TLS version and cipher suite");
                server12(hello,identity,wait);
            }
            handshakeComplete();
        } catch (const HttpError&) { abort(wait); throw; }
    }
    void send(View bytes,WaitContext& wait) {
        if (!complete_ || sentClose_) fail("TLS output is not open");
        for (std::size_t offset=0;offset<bytes.size;offset+=16384) {
            if (tls13_ && outgoing_.sequence>=(std::uint64_t{1}<<24)-1) updateWrite(wait,false);
            sendRecord(23,bytes.sub(offset,(std::min)(std::size_t{16384},bytes.size-offset)),wait);
        }
    }
    std::size_t receive(void* buffer,std::size_t capacity,WaitContext& wait) {
        if (!complete_) fail("TLS handshake has not completed");
        if (!capacity) return 0;
        unsigned empty=0;
        while (position_==application_.size()) {
            eraseSecret(application_); application_.clear(); position_=0;
            if (receivedClose_) return 0;
            auto record=readRecord(wait);
            if (!pending_.empty() && record.type!=22) fail("record interleaved with a fragmented handshake message");
            if (record.type==21) return 0;
            if (record.type==22 && tls13_) { postHandshake(record.body,wait); continue; }
            if (record.type!=23) fail("unexpected post-handshake record");
            if (record.body.empty() && ++empty>32) fail("too many empty application records");
            application_=std::move(record.body);
        }
        const auto size=(std::min)(capacity,application_.size()-position_);
        std::memcpy(buffer,application_.data()+position_,size); position_+=size; return size;
    }
    void finish(WaitContext& wait) {
        if (!sentClose_) {
            try { alert(1,0,wait); sentClose_=true; }
            catch (const NetworkError& error) {
                wait.check();
                if (error.kind()!=ErrorKind::System) throw;
                sentClose_=true; return;
            }
        }
        if (receivedClose_) return;
         
         
        auto grace=wait.boundedFor(std::chrono::milliseconds(50));
        struct Guard { bool& flag; ~Guard() { flag=false; } } guard{closing_}; closing_=true;
        try {
            for (unsigned n=0;n<64 && !receivedClose_;++n) {
                auto record=readRecord(grace);
                if (!pending_.empty() && record.type!=22) fail("record interleaved with a fragmented closing handshake");
                if (record.type==22 && tls13_) postHandshake(record.body,grace);
            }
        } catch (const ClosingTransportEnd&) {
            wait.check();
        } catch (const NetworkError& error) {
            wait.check();
            if (error.kind()!=ErrorKind::Timeout && error.kind()!=ErrorKind::System) throw;
        }
    }
private:
    void retryTranscript() {
        auto hash=Sha256::hash(transcript_); transcript_.clear(); Bytes synthetic{254}; integer(synthetic,hash.size(),3); append(synthetic,hash); remember(synthetic);
    }
    void abort(WaitContext& wait) noexcept {
        try { alert(2,80,wait); } catch (...) {}
    }
    void handshakeComplete() {
        if (!pending_.empty()) fail("handshake data crossed an encryption epoch");
        complete_=true; eraseSecret(privateKey_); privateKey_.clear(); eraseSecret(handshakeSecret_); handshakeSecret_.clear();
        eraseSecret(master_); master_.clear(); eraseSecret(clientHandshake_); clientHandshake_.clear(); eraseSecret(serverHandshake_); serverHandshake_.clear();
        transcript_.clear();
    }
    void establish13(View peer,WaitContext& wait) {
        wait.check(); auto shared=groupShared(group_,privateKey_,peer,wait); const Bytes zeros(32);
        auto early=hmac(zeros,zeros); auto derived=derive(early,"derived",{});
        handshakeSecret_=hmac(derived,shared); clientHandshake_=derive(handshakeSecret_,"c hs traffic",transcript_);
        serverHandshake_=derive(handshakeSecret_,"s hs traffic",transcript_);
        auto masterSalt=derive(handshakeSecret_,"derived",{}); master_=hmac(masterSalt,zeros);
        eraseSecret(shared); eraseSecret(early); eraseSecret(derived); eraseSecret(masterSalt); wait.check();
    }
    Bytes finished13(View secret) const { auto key=label(secret,"finished",{},32); auto result=hmac(key,Sha256::hash(transcript_)); eraseSecret(key); return result; }
    void encryptedExtensions(View message,bool offeredSni) {
        Reader reader(message); const auto ext=extensions(reader.vec(2)); reader.end();
        for (const auto& entry:ext) {
            if (entry.first==16) {
                Reader alpn(entry.second); Reader names(alpn.vec(2)); alpn.end();
                if (!equal(names.vec(1),View("http/1.1"))) fail("server selected an unsupported ALPN protocol");
                names.end();
            } else if (entry.first==0) { if (!offeredSni || !entry.second.empty()) fail("invalid SNI acknowledgment"); }
            else if (entry.first==10) {
                Reader groups(entry.second); const auto names=groups.vec(2); groups.end();
                if (!names.size || names.size%2) fail("invalid server supported-groups list");
            } else fail("unsolicited EncryptedExtensions entry "+std::to_string(entry.first));
        }
    }
    void client13(const ServerHello& hello,const HttpUrl& url,const HttpOptions& options,WaitContext& wait) {
        establish13(hello.publicKey,wait); traffic(incoming_,serverHandshake_); traffic(outgoing_,clientHandshake_);
        auto message=receiveHandshake(wait); encryptedExtensions(handshakeBody(message,8),!url.numeric); remember(message);
        bool requestedCertificate=false; message=receiveHandshake(wait);
        if (message[0]==13) {
            Reader request(handshakeBody(message,13)); if (request.vec(1).size) fail("unexpected handshake certificate-request context");
            const auto ext=extensions(request.vec(2)); request.end(); const auto* algorithms=extension(ext,13);
            if (!algorithms) fail("CertificateRequest omitted signature algorithms");
            Reader signatures(*algorithms); const auto names=signatures.vec(2); signatures.end();
            if (!names.size || names.size%2) fail("invalid requested signature algorithms");
            remember(message); requestedCertificate=true; message=receiveHandshake(wait);
        }
        const auto chain=peerCertificates(handshakeBody(message,11),true);
        verifyCertificates(chain,url,options.caFile,wait); remember(message);
        message=receiveHandshake(wait); Reader verify(handshakeBody(message,15)); const auto algorithm=verify.uint(2); const auto signature=verify.vec(2); verify.end();
        const auto context=verifyContext(transcript_);
        const bool valid=algorithm==0x0804 && chain[0].ecPoint.empty() ? verifyRsa(chain[0].key,context,signature,true,&wait)
            : ((algorithm==0x0403 && chain[0].ecPoint.size()==65) || (algorithm==0x0503 && chain[0].ecPoint.size()==97)) && verifyEcdsa(chain[0].ecPoint,context,signature,&wait,algorithm==0x0503?HashAlgorithm::Sha384:HashAlgorithm::Sha256);
        if (!valid) fail("server CertificateVerify signature is invalid",HttpErrorKind::Certificate);
        remember(message); message=receiveHandshake(wait);
        if (!equal(handshakeBody(message,20),finished13(serverHandshake_))) fail("server Finished authentication failed");
        remember(message);
        auto clientApplication=derive(master_,"c ap traffic",transcript_),serverApplication=derive(master_,"s ap traffic",transcript_);
        if (requestedCertificate) sendHandshake(11,Bytes{0,0,0,0},wait);
        sendHandshake(20,finished13(clientHandshake_),wait); traffic(outgoing_,clientApplication); traffic(incoming_,serverApplication);
        eraseSecret(clientApplication); eraseSecret(serverApplication);
    }
    void server13(const ClientHello& hello,const ServerIdentity& identity,WaitContext& wait) {
        const auto& peer=clientShare(hello,group_);
        if (!contains16(hello.groups,group_) || peer.size()!=groupKeyLength(group_) || !contains16(hello.signatureAlgorithms,0x0804)) fail("client did not offer a supported ECDHE share and RSA-PSS/SHA-256");
        Bytes answer=number(0x0303,2); append(answer,serverRandom_); append(answer,vector(session_,1)); integer(answer,0x1301,2); answer.push_back(0);
        Bytes ext; addExtension(ext,43,Bytes{3,4}); Bytes share=number(group_,2); append(share,vector(groupPublic(group_,privateKey_,wait),2)); addExtension(ext,51,share);
        append(answer,vector(ext,2)); sendHandshake(2,answer,wait); establish13(peer,wait);
        traffic(outgoing_,serverHandshake_); traffic(incoming_,clientHandshake_); ext.clear();
        if (hello.alpn) addExtension(ext,16,vector(vector(View("http/1.1"),1),2));
        if (hello.sni) addExtension(ext,0,{});
        sendHandshake(8,vector(ext,2),wait); sendHandshake(11,certificateMessage(identity,true),wait);
        const auto signature=signRsa(identity.key,verifyContext(transcript_),true,&wait); Bytes verify=number(0x0804,2); append(verify,vector(signature,2));
        sendHandshake(15,verify,wait); sendHandshake(20,finished13(serverHandshake_),wait);
        auto clientApplication=derive(master_,"c ap traffic",transcript_),serverApplication=derive(master_,"s ap traffic",transcript_);
        traffic(outgoing_,serverApplication); const auto message=receiveHandshake(wait);
        if (!equal(handshakeBody(message,20),finished13(clientHandshake_))) fail("client Finished authentication failed");
        remember(message); traffic(incoming_,clientApplication); eraseSecret(clientApplication); eraseSecret(serverApplication);
    }
    void establish12(View peer,bool client,WaitContext& wait) {
        wait.check(); auto shared=groupShared(group_,privateKey_,peer,wait);
        master_=prf(shared,"extended master secret",Sha256::hash(transcript_),48); eraseSecret(shared);
        Bytes seed=serverRandom_; append(seed,clientRandom_); auto keys=prf(master_,"key expansion",seed,40);
        eraseSecret(outgoing_.key); eraseSecret(outgoing_.iv); eraseSecret(incoming_.key); eraseSecret(incoming_.iv);
        outgoing_.key=View(keys).sub(client?0:16,16).copy(); incoming_.key=View(keys).sub(client?16:0,16).copy();
        outgoing_.iv=View(keys).sub(client?32:36,4).copy(); incoming_.iv=View(keys).sub(client?36:32,4).copy();
        outgoing_.enabled=incoming_.enabled=false; outgoing_.sequence=incoming_.sequence=0; eraseSecret(keys); wait.check();
    }
    Bytes finished12(bool client) const { return prf(master_,client?"client finished":"server finished",Sha256::hash(transcript_),12); }
    void client12(const HttpUrl& url,const HttpOptions& options,WaitContext& wait) {
        auto message=receiveHandshake(wait); const auto chain=peerCertificates(handshakeBody(message,11),false);
        verifyCertificates(chain,url,options.caFile,wait); remember(message);
        if ((cipher_==0xc02b)!=!chain[0].ecPoint.empty()) fail("server certificate does not match the cipher suite",HttpErrorKind::Certificate);
        message=receiveHandshake(wait); const auto body=handshakeBody(message,12); Reader exchange(body);
        if (exchange.uint(1)!=3) fail("server selected an unsupported TLS 1.2 curve type");
        group_=static_cast<unsigned>(exchange.uint(2)); if (group_!=0x1d && group_!=0x17 && group_!=0x18) fail("server selected an unsupported TLS 1.2 curve");
        const auto peer=exchange.vec(1).copy(); if (peer.size()!=groupKeyLength(group_)) fail("invalid TLS 1.2 ECDHE key");
        if (group_!=0x1d) { eraseSecret(privateKey_); privateKey_=EcCurve(group_==0x18).secret(); }
        const auto algorithm=exchange.uint(2); const auto signature=exchange.vec(2); exchange.end();
        Bytes signedData=clientRandom_; append(signedData,serverRandom_); append(signedData,body.sub(0,4+peer.size()));
        const bool valid=(algorithm==0x0403 || algorithm==0x0503) && !chain[0].ecPoint.empty() ? verifyEcdsa(chain[0].ecPoint,signedData,signature,&wait,algorithm==0x0503?HashAlgorithm::Sha384:HashAlgorithm::Sha256)
            : chain[0].ecPoint.empty() && (algorithm==0x0804 || algorithm==0x0401) && verifyRsa(chain[0].key,signedData,signature,algorithm==0x0804,&wait);
        if (!valid) fail("server key-exchange signature is invalid",HttpErrorKind::Certificate);
        remember(message); message=receiveHandshake(wait); bool requestedCertificate=false;
        if (message[0]==13) {
            Reader request(handshakeBody(message,13)); if (!request.vec(1).size) fail("empty requested certificate types");
            const auto algorithms=request.vec(2); if (!algorithms.size || algorithms.size%2) fail("invalid requested signature algorithms");
            Reader authorities(request.vec(2)); while (authorities.remaining()) { Der name(authorities.vec(2)); (void)name.node(0x30); name.end(); }
            request.end(); remember(message); requestedCertificate=true; message=receiveHandshake(wait);
        }
        Reader done(handshakeBody(message,14)); done.end(); remember(message);
        if (requestedCertificate) sendHandshake(11,Bytes{0,0,0},wait);
        sendHandshake(16,vector(groupPublic(group_,privateKey_,wait),1),wait); establish12(peer,true,wait);
        changeCipherSpec(wait,true); sendHandshake(20,finished12(true),wait);
        changeCipherSpec(wait,false); message=receiveHandshake(wait);
        if (!equal(handshakeBody(message,20),finished12(false))) fail("server TLS 1.2 Finished authentication failed");
        remember(message);
    }
    void server12(const ClientHello& hello,const ServerIdentity& identity,WaitContext& wait) {
        group_=contains16(hello.groups,0x1d)?0x1d:contains16(hello.groups,0x17)?0x17:contains16(hello.groups,0x18)?0x18:0;
        if (!hello.ems || !group_) fail("TLS 1.2 requires supported ECDHE and extended master secret");
        if (group_!=0x1d) { eraseSecret(privateKey_); privateKey_=EcCurve(group_==0x18).secret(); }
        const bool pss=contains16(hello.signatureAlgorithms,0x0804);
        if (!pss && !contains16(hello.signatureAlgorithms,0x0401)) fail("client did not offer RSA/SHA-256 signatures");
        static constexpr Byte marker[]={0x44,0x4f,0x57,0x4e,0x47,0x52,0x44,1}; std::copy(std::begin(marker),std::end(marker),serverRandom_.begin()+24);
        Bytes answer=number(0x0303,2); append(answer,serverRandom_); answer.push_back(0); integer(answer,0xc02f,2); answer.push_back(0);
        Bytes ext; addExtension(ext,23,{}); addExtension(ext,0xff01,Bytes{0});
        if (hello.sni) addExtension(ext,0,{});
        if (hello.alpn) addExtension(ext,16,vector(vector(View("http/1.1"),1),2));
        append(answer,vector(ext,2)); sendHandshake(2,answer,wait); sendHandshake(11,certificateMessage(identity,false),wait);
        Bytes exchange{3}; integer(exchange,group_,2); append(exchange,vector(groupPublic(group_,privateKey_,wait),1)); Bytes signedData=clientRandom_; append(signedData,serverRandom_); append(signedData,exchange);
        const auto signature=signRsa(identity.key,signedData,pss,&wait); integer(exchange,pss?0x0804:0x0401,2); append(exchange,vector(signature,2));
        sendHandshake(12,exchange,wait); sendHandshake(14,{},wait);
        auto message=receiveHandshake(wait); Reader key(handshakeBody(message,16)); const auto peer=key.vec(1).copy(); key.end();
        if (peer.size()!=groupKeyLength(group_)) fail("invalid TLS 1.2 client ECDHE key");
        remember(message); establish12(peer,false,wait); changeCipherSpec(wait,false); message=receiveHandshake(wait);
        if (!equal(handshakeBody(message,20),finished12(true))) fail("client TLS 1.2 Finished authentication failed");
        remember(message); changeCipherSpec(wait,true); sendHandshake(20,finished12(false),wait);
    }
    void updateWrite(WaitContext& wait,bool request) {
        sendHandshake(24,Bytes{static_cast<Byte>(request?1:0)},wait,false);
        auto next=label(outgoing_.secret,"traffic upd",{},32); traffic(outgoing_,next); eraseSecret(next);
    }
    void postHandshake(View bytes,WaitContext& wait) {
        if (bytes.size>1024*1024-pending_.size()) fail("post-handshake buffering exceeds bounds");
        append(pending_,bytes);
        while (pending_.size()>=4) {
            const auto length=(std::size_t(pending_[1])<<16)|(std::size_t(pending_[2])<<8)|pending_[3];
            if (length>65536) fail("post-handshake message exceeds bounds");
            if (pending_.size()<length+4) break;
            const Byte type=pending_[0]; Bytes message(pending_.begin()+4,pending_.begin()+length+4); pending_.erase(pending_.begin(),pending_.begin()+length+4);
            if (type==24) {
                if (message.size()!=1 || message[0]>1 || !pending_.empty()) fail("invalid KeyUpdate");
                auto next=label(incoming_.secret,"traffic upd",{},32); traffic(incoming_,next); eraseSecret(next);
                if (message[0] && !sentClose_) updateWrite(wait,false);
            } else if (type==4) {
                if (serverSide_) fail("a client cannot issue session tickets");
                if (++tickets_>64) fail("too many session tickets");
                Reader ticket(message); if (ticket.uint(4)>604800) fail("invalid session-ticket lifetime");
                (void)ticket.uint(4); (void)ticket.vec(1); if (!ticket.vec(2).size) fail("empty session ticket");
                (void)extensions(ticket.vec(2)); ticket.end();
            } else fail("unsupported post-handshake message");
        }
    }
    Bytes clientRandom_,serverRandom_,session_,privateKey_,handshakeSecret_,master_,clientHandshake_,serverHandshake_;
    Bytes application_;
    std::size_t position_=0;
    unsigned tickets_=0;
    unsigned cipher_=0;
    unsigned group_=0x1d;
    bool serverSide_=false;
};

}}}

#endif
namespace ncl { namespace detail {
 
class HttpTransport {
public:
    HttpTransport()=default;
    explicit HttpTransport(Socket socket) : socket_(std::move(socket)) {}
    HttpTransport(const HttpTransport&)=delete;
    HttpTransport& operator=(const HttpTransport&)=delete;
    void connect(addrinfo* addresses,WaitContext& wait) { socket_.connectResolved(addresses,wait); }
    void send(const void* data,std::size_t size,WaitContext& wait) {
#ifdef NCL_ENABLE_TLS
        if (tls_) { tls_->send(tls::View(static_cast<const unsigned char*>(data),size),wait); return; }
#endif
        rawSend(data,size,wait);
    }
    std::size_t receive(void* data,std::size_t size,WaitContext& wait) {
#ifdef NCL_ENABLE_TLS
        if (tls_) return tls_->receive(data,size,wait);
#endif
        return rawReceive(data,size,wait);
    }
    void startTls(const HttpUrl& url,const HttpOptions& options,WaitContext& wait) {
#ifdef NCL_ENABLE_TLS
        createTls(); tls_->client(url,options,wait);
#else
        (void)url; (void)options; (void)wait;
        httpFail(HttpErrorKind::Unsupported,"HTTPS requires NCL_ENABLE_TLS");
#endif
    }
    static void validateServerTls(const std::string& certificate,const std::string& key) {
#ifdef NCL_ENABLE_TLS
        (void)tls::serverIdentity(certificate,key);
#else
        (void)certificate; (void)key;
        httpFail(HttpErrorKind::Unsupported,"HTTPS server requires NCL_ENABLE_TLS");
#endif
    }
    void startServerTls(const std::string& certificate,const std::string& key,WaitContext& wait) {
#ifdef NCL_ENABLE_TLS
        createTls(); const auto identity=tls::serverIdentity(certificate,key); tls_->server(identity,wait);
#else
        (void)certificate; (void)key; (void)wait;
        httpFail(HttpErrorKind::Unsupported,"HTTPS server requires NCL_ENABLE_TLS");
#endif
    }
    void finish(WaitContext& wait) {
#ifdef NCL_ENABLE_TLS
        if (tls_) tls_->finish(wait);
#else
        (void)wait;
#endif
    }
private:
    void rawSend(const void* data,std::size_t size,WaitContext& wait) {
        std::size_t progress=0;
        try { socket_.sendAllImpl(data,size,wait,progress); }
        catch (const NetworkError& error) { socket_.transferError(error,progress); }
    }
    std::size_t rawReceive(void* data,std::size_t size,WaitContext& wait) {
        std::size_t progress=0;
        try { return socket_.receiveImpl(data,size,wait,progress); }
        catch (const NetworkError& error) { socket_.transferError(error,progress); }
    }
    Socket socket_;
#ifdef NCL_ENABLE_TLS
    void createTls() {
        if (tls_) httpFail(HttpErrorKind::InvalidArgument,"TLS was already configured on this connection");
        tls_=std::make_unique<tls::Connection>(
            [this](const void* data,std::size_t size,WaitContext& wait) { rawSend(data,size,wait); },
            [this](void* data,std::size_t size,WaitContext& wait) { return rawReceive(data,size,wait); });
    }
    std::unique_ptr<tls::Connection> tls_;
#endif
};

 


class HttpReader {
public:
    HttpReader(HttpTransport& transport, WaitContext& wait) : transport_(transport), wait_(wait) {}

    HttpResponse response(const HttpRequest& request, const HttpOptions& options) {
        HttpResponse result;
        std::size_t informational = 0;
        for (;;) {
            std::size_t budget = options.maxHeaderBytes;
            const auto status = line(budget);
            if (status.size() < 13 || (status.compare(0, 9, "HTTP/1.1 ") != 0 && status.compare(0, 9, "HTTP/1.0 ") != 0) ||
                status[9] < '1' || status[9] > '5' || status[10] < '0' || status[10] > '9' ||
                status[11] < '0' || status[11] > '9' || status[12] != ' ' || !fieldValue(status))
                httpFail(HttpErrorKind::InvalidResponse, "Invalid HTTP response status line");
            result.version = status.substr(0, 8);
            result.statusCode = (status[9] - '0') * 100 + (status[10] - '0') * 10 + status[11] - '0';
            result.reasonPhrase = status.substr(13);
            result.headers = headers(budget, false);
            if (result.statusCode == 101) httpFail(HttpErrorKind::Unsupported, "HTTP protocol upgrades are not supported");
            if (result.statusCode >= 200) break;
            if (++informational > options.maxInformationalResponses)
                httpFail(HttpErrorKind::InvalidResponse, "Too many informational HTTP responses");
            if (result.headers.contains("Content-Length") || result.headers.contains("Transfer-Encoding"))
                httpFail(HttpErrorKind::InvalidResponse, "Informational response must not specify body framing");
        }
        const auto lengths = result.headers.getAll("Content-Length");
        const auto encodings = result.headers.getAll("Transfer-Encoding");
         
        if (lengths.size() > 1 || encodings.size() > 1 || (!lengths.empty() && !encodings.empty()))
            httpFail(HttpErrorKind::InvalidResponse, "Ambiguous HTTP response framing");
        std::optional<std::size_t> length;
        if (!lengths.empty()) length = httpNumber(trimOws(lengths.front()), 10, HttpErrorKind::InvalidResponse);
        if (!encodings.empty() && result.version == "HTTP/1.0")
            httpFail(HttpErrorKind::InvalidResponse, "HTTP/1.0 response must not use Transfer-Encoding");
        if (request.method == "HEAD" || result.statusCode == 304) return result;
        if (result.statusCode == 204) {
            if (length || !encodings.empty()) httpFail(HttpErrorKind::InvalidResponse, "204 response must not specify body framing");
            return result;
        }
        if (!encodings.empty()) {
            if (!httpEqual(trimOws(encodings.front()), "chunked"))
                httpFail(HttpErrorKind::Unsupported, "Only chunked HTTP transfer coding is supported");
            std::size_t budget = options.maxChunkMetadataBytes;
            for (;;) {
                const auto chunk = line(budget);
                const auto separator = chunk.find_first_of("; \t");
                const auto size = httpNumber(std::string_view(chunk).substr(0, separator), 16, HttpErrorKind::InvalidResponse);
                if (separator != std::string::npos) chunkExtensions(std::string_view(chunk).substr(separator));
                if (size == 0) {
                    std::size_t trailerBudget = (std::min)(budget, options.maxHeaderBytes);
                    result.trailers = headers(trailerBudget, true);
                    break;
                }
                body(result.body, size, options.maxBodyBytes);
                if (!line(budget).empty()) httpFail(HttpErrorKind::InvalidResponse, "Missing CRLF after HTTP chunk");
            }
        } else if (length) {
            body(result.body, *length, options.maxBodyBytes);
        } else {
            while (available() || refill()) {
                wait_.check();
                appendBody(result.body, buffer_.data() + position_, available(), options.maxBodyBytes);
                position_ = end_;
            }
        }
        if (result.statusCode == 205 && !result.body.empty())
            httpFail(HttpErrorKind::InvalidResponse, "205 response must not contain content");
        return result;
    }
    HttpServerRequest request(const HttpOptions& options) {
        try {
            HttpServerRequest result;
            std::size_t budget = options.maxHeaderBytes;
            const auto start = line(budget);
            const auto first = start.find(' ');
            const auto second = first == std::string::npos ? first : start.find(' ', first + 1);
            if (first == std::string::npos || second == std::string::npos || !httpToken(std::string_view(start).substr(0, first)))
                httpFail(HttpErrorKind::InvalidRequest, "Invalid HTTP request line");
            result.method = start.substr(0, first);
            result.target = start.substr(first + 1, second - first - 1);
            result.version = start.substr(second + 1);
            if (result.version != "HTTP/1.1" && result.version != "HTTP/1.0")
                httpFail(HttpErrorKind::UnsupportedVersion, "Unsupported HTTP request version");
            parseServerTarget(result);
            result.headers = headers(budget, false);
            const auto hosts = result.headers.getAll("Host");
            if ((result.version == "HTTP/1.1" && hosts.size() != 1) || hosts.size() > 1)
                httpFail(HttpErrorKind::InvalidRequest, "HTTP/1.1 needs exactly one Host header");
            if (!hosts.empty()) {
                if (hosts[0].empty() || hosts[0].find_first_of("/?#@\\ \t") != std::string::npos)
                    httpFail(HttpErrorKind::InvalidRequest, "Invalid HTTP Host header");
                try { (void)parseHttpUrl("http://" + hosts[0]); }
                catch (const HttpError&) { httpFail(HttpErrorKind::InvalidRequest, "Invalid HTTP Host header"); }
            }
            const auto lengths = result.headers.getAll("Content-Length");
            const auto encodings = result.headers.getAll("Transfer-Encoding");
            if (lengths.size() > 1 || encodings.size() > 1 || (!lengths.empty() && !encodings.empty()))
                httpFail(HttpErrorKind::InvalidRequest, "Ambiguous HTTP request framing");
            if (result.headers.contains("Expect"))
                httpFail(HttpErrorKind::ExpectationFailed, "HTTP Expect is not supported");
            if (result.headers.contains("Upgrade") || result.method == "CONNECT")
                httpFail(HttpErrorKind::Unsupported, "HTTP upgrades and tunnels are not supported");
            if (!encodings.empty()) {
                if (result.version == "HTTP/1.0" || !httpEqual(trimOws(encodings.front()), "chunked"))
                    httpFail(HttpErrorKind::Unsupported, "Unsupported HTTP request transfer coding");
                std::size_t metadata = options.maxChunkMetadataBytes;
                for (;;) {
                    const auto chunk = line(metadata);
                    const auto separator = chunk.find_first_of("; \t");
                    const auto size = httpNumber(std::string_view(chunk).substr(0, separator), 16, HttpErrorKind::InvalidRequest);
                    if (separator != std::string::npos) chunkExtensions(std::string_view(chunk).substr(separator));
                    if (size == 0) {
                        std::size_t remaining = (std::min)(metadata, options.maxHeaderBytes);
                        result.trailers = headers(remaining, true);
                        break;
                    }
                    body(result.body, size, options.maxBodyBytes);
                    if (!line(metadata).empty()) httpFail(HttpErrorKind::InvalidRequest, "Missing CRLF after request chunk");
                }
            } else if (!lengths.empty()) {
                body(result.body, httpNumber(trimOws(lengths[0]), 10, HttpErrorKind::InvalidRequest), options.maxBodyBytes);
            }
            return result;
        } catch (const HttpError& error) {
            if (error.kind() == HttpErrorKind::InvalidResponse)
                throw HttpError(HttpErrorKind::InvalidRequest, error.what());
            throw;
        }
    }
private:
    static void chunkExtensions(std::string_view value) {
        std::size_t i = 0;
        const auto whitespace = [&] { while (i < value.size() && (value[i] == ' ' || value[i] == '\t')) ++i; };
        while (i < value.size()) {
            whitespace();
            if (i == value.size() || value[i++] != ';') httpFail(HttpErrorKind::InvalidResponse, "Invalid HTTP chunk extension");
            whitespace();
            const auto start = i;
            while (i < value.size() && tokenChar(static_cast<unsigned char>(value[i]))) ++i;
            if (i == start) httpFail(HttpErrorKind::InvalidResponse, "Empty HTTP chunk extension name");
            whitespace();
            if (i < value.size() && value[i] == '=') {
                ++i; whitespace();
                if (i < value.size() && value[i] == '"') {
                    ++i;
                    bool closed = false;
                    while (i < value.size()) {
                        const unsigned char ch = static_cast<unsigned char>(value[i++]);
                        if (ch == '"') { closed = true; break; }
                        if (ch == '\\') {
                            if (i == value.size()) break;
                            const unsigned char escaped = static_cast<unsigned char>(value[i++]);
                            if ((escaped < 32 && escaped != '\t') || escaped == 127)
                                httpFail(HttpErrorKind::InvalidResponse, "Invalid escaped HTTP chunk extension");
                        } else if ((ch < 32 && ch != '\t') || ch == 127) {
                            httpFail(HttpErrorKind::InvalidResponse, "Invalid quoted HTTP chunk extension");
                        }
                    }
                    if (!closed) httpFail(HttpErrorKind::InvalidResponse, "Unclosed HTTP chunk extension");
                } else {
                    const auto token = i;
                    while (i < value.size() && tokenChar(static_cast<unsigned char>(value[i]))) ++i;
                    if (i == token) httpFail(HttpErrorKind::InvalidResponse, "Empty HTTP chunk extension value");
                }
            }
        }
    }
    std::size_t available() const noexcept { return end_ - position_; }
    bool refill() {
        position_ = 0;
        end_ = transport_.receive(buffer_.data(), buffer_.size(), wait_);
        return end_ != 0;
    }
    char byte(std::size_t& budget) {
        wait_.check();
        if (budget == 0) httpFail(HttpErrorKind::HeadersTooLarge, "HTTP header or chunk metadata limit exceeded");
        if (!available() && !refill()) httpFail(HttpErrorKind::InvalidResponse, "EOF inside HTTP headers or chunk metadata");
        --budget;
        return buffer_[position_++];
    }
    std::string line(std::size_t& budget) {
        std::string result;
        for (;;) {
            const char ch = byte(budget);
            if (ch == '\r') {
                if (byte(budget) != '\n') httpFail(HttpErrorKind::InvalidResponse, "HTTP requires CRLF line endings");
                return result;
            }
            if (ch == '\n') httpFail(HttpErrorKind::InvalidResponse, "Bare LF in HTTP message");
            result += ch;
        }
    }
    HttpHeaders headers(std::size_t& budget, bool trailers) {
        HttpHeaders result;
        for (;;) {
            const auto field = line(budget);
            if (field.empty()) return result;
            const auto colon = field.find(':');
            const auto name = std::string_view(field).substr(0, colon);
            if (colon == std::string::npos || !httpToken(name) || !fieldValue(field))
                httpFail(HttpErrorKind::InvalidResponse, "Invalid HTTP header or obsolete line folding");
            if (trailers) {
                for (auto forbidden : {"Content-Length", "Transfer-Encoding", "Host", "Connection", "Trailer", "Upgrade"})
                    if (httpEqual(name, forbidden)) httpFail(HttpErrorKind::InvalidResponse, "Forbidden HTTP trailer field");
            }
            result.add(std::string(name), std::string(trimOws(std::string_view(field).substr(colon + 1))));
        }
    }
    static void appendBody(std::string& target, const char* data, std::size_t count, std::size_t maximum) {
        if (count > maximum - target.size()) httpFail(HttpErrorKind::BodyTooLarge, "HTTP response body exceeds the configured limit");
        target.append(data, count);
    }
    void body(std::string& target, std::size_t count, std::size_t maximum) {
        if (count > maximum - target.size()) httpFail(HttpErrorKind::BodyTooLarge, "HTTP response body exceeds the configured limit");
        while (count != 0) {
            wait_.check();
            if (!available() && !refill()) httpFail(HttpErrorKind::InvalidResponse, "EOF inside HTTP response body");
            const auto taken = (std::min)(count, available());
            appendBody(target, buffer_.data() + position_, taken, maximum);
            position_ += taken;
            count -= taken;
        }
    }
    HttpTransport& transport_;
    WaitContext& wait_;
    std::array<char, 8192> buffer_{};
    std::size_t position_ = 0;
    std::size_t end_ = 0;
};

}  

 
class HttpClient {
public:
    static constexpr bool supportsHttps() noexcept {
#ifdef NCL_ENABLE_TLS
        return true;
#else
        return false;
#endif
    }
    HttpResponse request(const std::string& address, const HttpRequest& request, HttpOptions options = {}) const {
        detail::validateWaitOptions(options.wait);
        const auto url = detail::parseHttpUrl(address);
        if (url.secure && !supportsHttps())
            detail::httpFail(HttpErrorKind::Unsupported, "HTTPS requires NCL_ENABLE_TLS");
        const auto outgoing = detail::serializeHttpRequest(url, request, options);
        auto addresses = detail::resolve(url.host, url.port, SOCK_STREAM, detail::nativeFamily(options.family));
        detail::WaitContext wait(options.wait);  
        detail::HttpTransport transport;
        transport.connect(addresses.get(), wait);
        if (url.secure) transport.startTls(url, options, wait);
        transport.send(outgoing.data(), outgoing.size(), wait);
        transport.send(request.body.data(), request.body.size(), wait);
        detail::HttpReader reader(transport, wait);
        auto response = reader.response(request, options);
        wait.check();
        transport.finish(wait);
        return response;
    }
    HttpResponse get(const std::string& url, HttpOptions options = {}) const {
        return request(url, HttpRequest{}, std::move(options));
    }
    HttpResponse head(const std::string& url, HttpOptions options = {}) const {
        HttpRequest message;
        message.method = "HEAD";
        return request(url, message, std::move(options));
    }
    HttpResponse post(const std::string& url, std::string body, std::string contentType = "application/octet-stream", HttpOptions options = {}) const {
        HttpRequest message;
        message.method = "POST";
        message.headers.add("Content-Type", std::move(contentType));
        message.body = std::move(body);
        return request(url, message, std::move(options));
    }
};

namespace detail {
inline std::string httpJsonString(std::string_view text) {
     
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto ch = static_cast<unsigned char>(text[i]);
        if (ch < 128) continue;
        const std::size_t extra = ch >= 0xc2 && ch <= 0xdf ? 1 : ch >= 0xe0 && ch <= 0xef ? 2 : ch >= 0xf0 && ch <= 0xf4 ? 3 : 0;
        if (extra == 0 || extra > text.size() - i - 1) httpFail(HttpErrorKind::InvalidArgument, "Invalid UTF-8 in HTTP metadata");
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if ((ch == 0xe0 && second < 0xa0) || (ch == 0xed && second >= 0xa0) ||
            (ch == 0xf0 && second < 0x90) || (ch == 0xf4 && second >= 0x90))
            httpFail(HttpErrorKind::InvalidArgument, "Invalid UTF-8 in HTTP metadata");
        for (std::size_t n = 1; n <= extra; ++n)
            if ((static_cast<unsigned char>(text[i + n]) & 0xc0) != 0x80)
                httpFail(HttpErrorKind::InvalidArgument, "Invalid UTF-8 in HTTP metadata");
        i += extra;
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 32 || ch == '<' || ch == '>' || ch == '&') {
            result += "\\u00"; result += digits[ch >> 4]; result += digits[ch & 15];
        } else result += static_cast<char>(ch);
    }
    return result + '"';
}
inline const char* httpReason(int status) noexcept {
    switch (status) {
        case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted";
        case 204: return "No Content"; case 205: return "Reset Content"; case 301: return "Moved Permanently";
        case 302: return "Found"; case 304: return "Not Modified"; case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect"; case 400: return "Bad Request"; case 401: return "Unauthorized";
        case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
        case 408: return "Request Timeout"; case 409: return "Conflict"; case 413: return "Content Too Large";
        case 414: return "URI Too Long"; case 415: return "Unsupported Media Type"; case 417: return "Expectation Failed";
        case 422: return "Unprocessable Content"; case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large"; case 500: return "Internal Server Error";
        case 501: return "Not Implemented"; case 503: return "Service Unavailable"; case 505: return "HTTP Version Not Supported";
        default: return "";
    }
}
inline std::string httpDate() {
    const auto now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    if (::gmtime_s(&utc, &now) != 0) return {};
#else
    if (::gmtime_r(&now, &utc) == nullptr) return {};
#endif
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char value[64]{};
    std::snprintf(value, sizeof(value), "%s, %02d %s %04d %02d:%02d:%02d GMT", days[utc.tm_wday], utc.tm_mday, months[utc.tm_mon], utc.tm_year + 1900, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return value;
}
inline std::string serializeHttpResponse(const HttpServerRequest& request, const HttpResponse& response, const HttpOptions& options) {
    if (response.statusCode < 200 || response.statusCode > 599 || !fieldValue(response.reasonPhrase) ||
        response.statusCode == 101 || response.trailers.size() != 0)
        httpFail(HttpErrorKind::InvalidArgument, "Invalid or unsupported HTTP server response");
    if (response.body.size() > options.maxBodyBytes) httpFail(HttpErrorKind::BodyTooLarge, "HTTP server response exceeds the body limit");
    if ((response.statusCode == 204 || response.statusCode == 205 || response.statusCode == 304) && !response.body.empty())
        httpFail(HttpErrorKind::InvalidArgument, "This HTTP status must not include response content");
    const auto reason = response.reasonPhrase.empty() ? std::string(httpReason(response.statusCode)) : response.reasonPhrase;
    std::string result;
    const auto append = [&](std::string_view value) {
        if (value.size() > options.maxHeaderBytes - result.size()) httpFail(HttpErrorKind::HeadersTooLarge, "HTTP server response headers exceed the limit");
        result.append(value.data(), value.size());
    };
    append(request.version == "HTTP/1.0" ? "HTTP/1.0 " : "HTTP/1.1 ");
    append(std::to_string(response.statusCode)); append(" "); append(reason); append("\r\nConnection: close\r\n");
    if (!response.headers.contains("Date")) {
        const auto date = httpDate();
        if (!date.empty()) { append("Date: "); append(date); append("\r\n"); }
    }
    for (const auto& field : response.headers) {
        for (auto managed : {"Content-Length", "Transfer-Encoding", "Connection", "Trailer", "Upgrade"})
            if (httpEqual(field.first, managed)) httpFail(HttpErrorKind::InvalidArgument, "This response header is managed by NCL");
        append(field.first); append(": "); append(field.second); append("\r\n");
    }
    if (response.statusCode != 204 && response.statusCode != 304) {
        append("Content-Length: "); append(std::to_string(response.body.size())); append("\r\n");
    }
    append("\r\n");
    return result;
}
inline std::string httpHtml(std::string_view text) {
    std::string result;
    for (char ch : text) {
        switch (ch) {
            case '&': result += "&amp;"; break; case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break; case '"': result += "&quot;"; break;
            case '\'': result += "&#39;"; break; default: result += ch;
        }
    }
    return result;
}
}  

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpServerRequest&)>;
    explicit HttpServer(std::uint16_t port = 8080, const std::string& bindAddress = "127.0.0.1", HttpOptions options = {}, int backlog = 16, IPv6Mode mode = IPv6Mode::IPv6Only)
        : listener_(port, bindAddress, backlog, mode), options_(std::move(options)) {
        detail::validateWaitOptions(options_.wait);
    }
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) noexcept = default;
    HttpServer& operator=(HttpServer&&) noexcept = default;
    Endpoint localEndpoint() const { return listener_.localEndpoint(); }
    bool isOpen() const noexcept { return listener_.isOpen(); }
    void close() noexcept { listener_.close(); }  

    void route(std::string method, std::string path, Handler handler, HttpRouteOptions options = {}) {
        configurable();
        if (!handler) detail::httpFail(HttpErrorKind::InvalidArgument, "An HTTP route needs a handler");
        bool known = false;
        for (auto allowed : {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"}) if (method == allowed) known = true;
        if (!known)
            detail::httpFail(HttpErrorKind::InvalidArgument, "Unsupported HTTP route method");
        HttpServerRequest target;
        target.method = method; target.target = path;
        try { detail::parseServerTarget(target); }
        catch (const HttpError&) { detail::httpFail(HttpErrorKind::InvalidArgument, "Invalid HTTP route path"); }
        if (target.path == "*" || !target.query.empty() || path.find('?') != std::string::npos ||
            (swagger_ && reserved(target.path)))
            detail::httpFail(HttpErrorKind::InvalidArgument, "Routes need a path without query parameters");
        for (std::size_t i = 0; i + 2 < path.size(); ++i) if (path[i] == '%') {
            const int decoded = detail::hexDigit(path[i + 1]) * 16 + detail::hexDigit(path[i + 2]);
            if (decoded == '{' || decoded == '}') detail::httpFail(HttpErrorKind::InvalidArgument, "Route template braces must not be percent encoded");
        }
        for (std::size_t start = path.find('{'); start != std::string::npos; start = path.find('{', start + 1)) {
            const auto end = path.find('}', start);
            if (end != std::string::npos && path.substr(start, end - start).find('%') != std::string::npos)
                detail::httpFail(HttpErrorKind::InvalidArgument, "Route parameter names must not be percent encoded");
        }
        Route entry{std::move(method), std::move(path), std::move(handler), std::move(options), std::move(target.pathSegments), {}, 0};
        for (const auto& segment : entry.segments) {
            if (segment.size() >= 2 && segment.front() == '{' && segment.back() == '}') {
                const auto name = segment.substr(1, segment.size() - 2);
                if (!detail::httpToken(name) || name.find_first_of("{}") != std::string::npos ||
                    std::find(entry.parameters.begin(), entry.parameters.end(), name) != entry.parameters.end())
                    detail::httpFail(HttpErrorKind::InvalidArgument, "Invalid or repeated HTTP path parameter");
                entry.parameters.push_back(name);
            } else {
                if (segment.find_first_of("{}") != std::string::npos)
                    detail::httpFail(HttpErrorKind::InvalidArgument, "Path parameters must occupy an entire segment");
                ++entry.literals;
            }
        }
        if (entry.options.successStatus < 200 || entry.options.successStatus > 599)
            detail::httpFail(HttpErrorKind::InvalidArgument, "Invalid documented HTTP response status");
        (void)detail::httpJsonString(entry.options.summary);
        (void)detail::httpJsonString(entry.options.requestContentType);
        (void)detail::httpJsonString(entry.options.responseContentType);
        for (std::size_t i = 0; i < entry.options.queryParameters.size(); ++i) {
            const auto& name = entry.options.queryParameters[i];
            (void)detail::httpJsonString(name);
            if (name.empty() || std::find(entry.options.queryParameters.begin(), entry.options.queryParameters.begin() + static_cast<std::ptrdiff_t>(i), name) != entry.options.queryParameters.begin() + static_cast<std::ptrdiff_t>(i))
                detail::httpFail(HttpErrorKind::InvalidArgument, "Empty or repeated documented query parameter");
        }
        if (swagger_ && reserved(entry.path)) detail::httpFail(HttpErrorKind::InvalidArgument, "Route conflicts with Swagger endpoints");
        for (const auto& other : routes_) {
            if (!overlap(entry, other) || entry.literals != other.literals) continue;
            if (entry.path != other.path || entry.method == other.method)
                detail::httpFail(HttpErrorKind::InvalidArgument, "Duplicate or ambiguous HTTP route");
        }
        routes_.push_back(std::move(entry));
    }
    void get(std::string path, Handler handler, HttpRouteOptions options = {}) { route("GET", std::move(path), std::move(handler), std::move(options)); }
    void post(std::string path, Handler handler, HttpRouteOptions options = {}) { route("POST", std::move(path), std::move(handler), std::move(options)); }
    void put(std::string path, Handler handler, HttpRouteOptions options = {}) { route("PUT", std::move(path), std::move(handler), std::move(options)); }
    void patch(std::string path, Handler handler, HttpRouteOptions options = {}) { route("PATCH", std::move(path), std::move(handler), std::move(options)); }
    void remove(std::string path, Handler handler, HttpRouteOptions options = {}) { route("DELETE", std::move(path), std::move(handler), std::move(options)); }

    void enableTls(std::string certificateFile, std::string privateKeyFile) {
        configurable();
        detail::HttpTransport::validateServerTls(certificateFile, privateKeyFile);
        certificate_ = std::move(certificateFile); key_ = std::move(privateKeyFile);
    }
    void enableSwagger(std::string title = "NCL API", std::string version = "1.0.0", std::string assetsBase = "https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.34.1") {
        configurable();
        if (title.empty() || version.empty() || assetsBase.empty()) detail::httpFail(HttpErrorKind::InvalidArgument, "Swagger needs a title, version and asset URL");
        (void)detail::httpJsonString(title); (void)detail::httpJsonString(version);
        for (unsigned char ch : assetsBase) if (ch <= 32 || ch >= 127) detail::httpFail(HttpErrorKind::InvalidArgument, "Invalid Swagger asset URL");
        if (assetsBase.front() != '/' && assetsBase.rfind("https://", 0) != 0 && assetsBase.rfind("http://", 0) != 0)
            detail::httpFail(HttpErrorKind::InvalidArgument, "Swagger asset URL must use HTTP(S) or an absolute local path");
        if (assetsBase.rfind("//", 0) == 0) detail::httpFail(HttpErrorKind::InvalidArgument, "Protocol-relative Swagger asset URLs are not supported");
        for (const auto& entry : routes_) {
            HttpServerRequest parsed; parsed.method = "GET"; parsed.target = entry.path; detail::parseServerTarget(parsed);
            if (reserved(parsed.path)) detail::httpFail(HttpErrorKind::InvalidArgument, "Route conflicts with Swagger endpoints");
        }
        while (assetsBase.size() > 1 && assetsBase.back() == '/') assetsBase.pop_back();
        title_ = std::move(title); version_ = std::move(version); assets_ = std::move(assetsBase); swagger_ = true;
    }

    std::string openApi() const {
        const auto quote = detail::httpJsonString;
        std::string document = "{\"openapi\":\"3.0.3\",\"info\":{\"title\":" + quote(title_) + ",\"version\":" + quote(version_) +
            "},\"servers\":[{\"url\":\"/\"}],\"paths\":{";
        bool firstPath = true;
        for (std::size_t i = 0; i < routes_.size(); ++i) {
            bool seen = false;
            for (std::size_t j = 0; j < i; ++j) if (routes_[j].path == routes_[i].path) seen = true;
            if (seen) continue;
            if (!firstPath) document += ',';
            firstPath = false;
            document += quote(routes_[i].path) + ":{";
            bool firstMethod = true;
            for (std::size_t j = i; j < routes_.size(); ++j) {
                const auto& entry = routes_[j];
                if (entry.path != routes_[i].path) continue;
                if (!firstMethod) document += ',';
                firstMethod = false;
                std::string method = entry.method;
                for (auto& ch : method) ch = detail::asciiLower(ch);
                document += quote(method) + ":{\"operationId\":\"ncl_" + std::to_string(j) + "\",\"summary\":" + quote(entry.options.summary) + ",\"parameters\":[";
                bool firstParameter = true;
                for (const auto& name : entry.parameters) {
                    if (!firstParameter) document += ',';
                    firstParameter = false;
                    document += "{\"name\":" + quote(name) + ",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}";
                }
                for (const auto& name : entry.options.queryParameters) {
                    if (!firstParameter) document += ',';
                    firstParameter = false;
                    document += "{\"name\":" + quote(name) + ",\"in\":\"query\",\"required\":false,\"schema\":{\"type\":\"string\"}}";
                }
                document += ']';
                if (!entry.options.requestContentType.empty()) {
                    document += ",\"requestBody\":{\"required\":" + std::string(entry.options.requestBodyRequired ? "true" : "false") +
                        ",\"content\":{" + quote(entry.options.requestContentType) + ":{\"schema\":" + schema(entry.options.requestContentType) + "}}}";
                }
                document += ",\"responses\":{" + quote(std::to_string(entry.options.successStatus)) + ":{\"description\":\"Response\"";
                if (!entry.options.responseContentType.empty() && entry.options.successStatus != 204 && entry.options.successStatus != 304)
                    document += ",\"content\":{" + quote(entry.options.responseContentType) + ":{\"schema\":" + schema(entry.options.responseContentType) + "}}";
                document += "},\"default\":{\"description\":\"Error response\"}}}";
            }
            document += '}';
        }
        return document + "}}";
    }

    void serveOnce(WaitOptions acceptWait = {}) {
        configurable();
        struct Guard { bool& flag; ~Guard() { flag = false; } } guard{serving_};
        serving_ = true;
        auto socket = listener_.accept(acceptWait);
        const auto remote = socket.remoteEndpoint();
        auto options = options_;
        if (acceptWait.cancellation.canBeCancelled()) options.wait.cancellation = acceptWait.cancellation;
        detail::WaitContext wait(options.wait);
        detail::HttpTransport transport(std::move(socket));
        bool ready = certificate_.empty();
        bool reading = true;
        bool started = false;
        HttpServerRequest request;
        try {
            if (!ready) { transport.startServerTls(certificate_, key_, wait); ready = true; }
            detail::HttpReader reader(transport, wait);
            request = reader.request(options);
            request.remote = remote;
            reading = false;
            wait.check();
            HttpResponse response;
            try { response = dispatch(request); }
            catch (const std::bad_alloc&) { throw; }
            catch (...) { response = HttpResponse::text("Internal Server Error", 500); }
            wait.check();
            reply(transport, request, response, options, wait, started);
        } catch (const HttpError& error) {
            if (!ready || started) return;
            int status = 500;
            if (reading) {
                switch (error.kind()) {
                    case HttpErrorKind::InvalidRequest: status = 400; break;
                    case HttpErrorKind::HeadersTooLarge: status = 431; break;
                    case HttpErrorKind::BodyTooLarge: status = 413; break;
                    case HttpErrorKind::Unsupported: status = 501; break;
                    case HttpErrorKind::UnsupportedVersion: status = 505; break;
                    case HttpErrorKind::ExpectationFailed: status = 417; break;
                    default: break;
                }
            }
            try {
                const HttpOptions errorLimits;
                reply(transport, request, HttpResponse::text(detail::httpReason(status), status), errorLimits, wait, started);
            } catch (const NetworkError&) {
                if (options.wait.cancellation.isCancellationRequested()) throw;
            } catch (const HttpError&) {   }
        } catch (const NetworkError&) {
            if (options.wait.cancellation.isCancellationRequested()) throw;
             
        }
    }
    void serve(CancellationToken cancellation = {}) {
        try {
            while (isOpen()) serveOnce({std::chrono::milliseconds{0}, cancellation});
        } catch (const NetworkError& error) {
            if (error.kind() != ErrorKind::Cancelled || !cancellation.isCancellationRequested()) throw;
        }
    }
private:
    void configurable() const {
        if (serving_) detail::httpFail(HttpErrorKind::InvalidArgument, "HTTP server configuration cannot change while serving");
    }
    struct Route {
        std::string method, path;
        Handler handler;
        HttpRouteOptions options;
        std::vector<std::string> segments, parameters;
        std::size_t literals;
    };
    static bool parameter(const std::string& segment) { return segment.size() >= 2 && segment.front() == '{'; }
    static bool reserved(std::string_view path) { return path == "/openapi.json" || path == "/swagger" || path == "/swagger/" || path == "/swagger/index.html"; }
    static bool overlap(const Route& a, const Route& b) {
        if (a.segments.size() != b.segments.size()) return false;
        for (std::size_t i = 0; i < a.segments.size(); ++i)
            if (!parameter(a.segments[i]) && !parameter(b.segments[i]) && a.segments[i] != b.segments[i]) return false;
        return true;
    }
    static bool matches(const Route& route, const HttpServerRequest& request) {
        if (route.segments.size() != request.pathSegments.size()) return false;
        for (std::size_t i = 0; i < route.segments.size(); ++i) {
            if (parameter(route.segments[i])) { if (request.pathSegments[i].empty()) return false; }
            else if (route.segments[i] != request.pathSegments[i]) return false;
        }
        return true;
    }
    static std::string schema(std::string_view type) {
        return type == "application/json" || (type.size() >= 5 && type.substr(type.size() - 5) == "+json") ? "{}" : "{\"type\":\"string\"}";
    }
    static void reply(detail::HttpTransport& transport, const HttpServerRequest& request, const HttpResponse& response, const HttpOptions& options, detail::WaitContext& wait, bool& started) {
        const auto headers = detail::serializeHttpResponse(request, response, options);
        started = true;
        transport.send(headers.data(), headers.size(), wait);
        if (request.method != "HEAD" && response.statusCode != 204 && response.statusCode != 304)
            transport.send(response.body.data(), response.body.size(), wait);
        transport.finish(wait);
    }
    HttpResponse documentation(const HttpServerRequest& request) const {
        if (request.method == "OPTIONS") {
            auto response = HttpResponse::empty(); response.headers.set("Allow", "GET, HEAD, OPTIONS"); return response;
        }
        if (request.method != "GET" && request.method != "HEAD") {
            auto response = HttpResponse::text("Method Not Allowed", 405); response.headers.set("Allow", "GET, HEAD, OPTIONS"); return response;
        }
        if (request.path == "/openapi.json") return HttpResponse::json(openApi());
        const auto html = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>" + detail::httpHtml(title_) + "</title><link rel=\"stylesheet\" href=\"" + detail::httpHtml(assets_ + "/swagger-ui.css") +
            "\"></head><body><div id=\"swagger-ui\"></div><script src=\"" + detail::httpHtml(assets_ + "/swagger-ui-bundle.js") +
            "\"></script><script>window.onload=function(){SwaggerUIBundle({url:'/openapi.json',dom_id:'#swagger-ui',validatorUrl:null,"
            "deepLinking:true,persistAuthorization:false});};</script></body></html>";
        auto response = HttpResponse::text(html);
        response.headers.set("Content-Type", "text/html; charset=utf-8");
        return response;
    }
    HttpResponse dispatch(HttpServerRequest& request) const {
        if (swagger_ && reserved(request.path)) return documentation(request);
        std::size_t best = 0;
        bool found = false;
        for (const auto& route : routes_) if (matches(route, request) && (!found || route.literals > best)) { found = true; best = route.literals; }
        if (!found && request.path != "*") return HttpResponse::text("Not Found", 404);
        std::vector<std::string> allowed;
        const Route* selected = nullptr;
        for (const auto& route : routes_) {
            if (request.path != "*" && (!matches(route, request) || route.literals != best)) continue;
            if (std::find(allowed.begin(), allowed.end(), route.method) == allowed.end()) allowed.push_back(route.method);
            if (route.method == "GET" && std::find(allowed.begin(), allowed.end(), "HEAD") == allowed.end()) allowed.push_back("HEAD");
            if (request.path != "*" && route.method == request.method) selected = &route;
            else if (request.path != "*" && !selected && request.method == "HEAD" && route.method == "GET") selected = &route;
        }
        if (std::find(allowed.begin(), allowed.end(), "OPTIONS") == allowed.end()) allowed.push_back("OPTIONS");
        if (!selected || request.path == "*") {
            auto response = request.method == "OPTIONS" ? HttpResponse::empty() : HttpResponse::text("Method Not Allowed", 405);
            std::string value;
            for (const auto& method : allowed) { if (!value.empty()) value += ", "; value += method; }
            response.headers.set("Allow", value);
            return response;
        }
        for (std::size_t i = 0; i < selected->segments.size(); ++i) {
            const auto& segment = selected->segments[i];
            if (parameter(segment)) request.pathParameters.emplace_back(segment.substr(1, segment.size() - 2), request.pathSegments[i]);
        }
        return selected->handler(request);
    }
    ServerSocket listener_;
    HttpOptions options_;
    std::vector<Route> routes_;
    std::string certificate_, key_;
    bool swagger_ = false;
    bool serving_ = false;
    std::string title_ = "NCL API", version_ = "1.0.0", assets_;
};

}  
#endif  
