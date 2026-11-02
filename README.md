# NCL - Network Communication Library

![NCL - Network Communication Library](assets/ncl-banner.png)

[![C++: 17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](#1-build-and-run)
[![Header-only](https://img.shields.io/badge/header--only-7B68EE)](NCL.hpp)
[![Version: 0.3](https://img.shields.io/badge/version-0.3-7B68EE)](NCL.hpp)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Platforms: Windows | Linux](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-0078D4)](#14-compatibility-and-implementation)

NCL is a C++ library for TCP and UDP communication on Windows and Linux. The
whole library is in [NCL.hpp](NCL.hpp). It needs C++17 or later and has no
third-party dependencies. Its socket classes are inspired by Java's networking
API.

This is source version **V0.3**, still in development. Calls wait for their
operation to finish, fail, time out or be cancelled. Without options, there is
no NCL time limit.

NCL provides:

- TCP servers and clients, with byte operations and complete-message helpers.
- UDP datagrams with the sender's address and port.
- IPv4, IPv6 and explicit IPv6-only or dual-stack binding.
- Timeouts for complete socket operations and cooperative cancellation.
- Separate TCP input and output shutdown.
- Automatic socket cleanup, exclusive ownership and readable exceptions.

## Files

| File | Purpose |
| --- | --- |
| [NCL.hpp](NCL.hpp) | The complete library. Copy this file into your project. |
| [server_example.cpp](server_example.cpp) | A TCP server that reads one message and replies. |
| [client_example.cpp](client_example.cpp) | A TCP client that sends one message and reads the reply. |
| [README.md](README.md) | This guide and the public API reference. |
| [LICENSE](LICENSE) | The MIT license. |

The `assets/` folder contains the README banner in [PNG](assets/ncl-banner.png)
and [SVG](assets/ncl-banner.svg) formats. This README uses the PNG version.

All public types are in the `ncl` namespace. Examples use names such as
`ncl::Socket` and `ncl::DatagramPacket` directly. NCL selects its Windows or
Linux implementation internally, so application code can use the same API on
both systems.

## Contents

- [1. Build and run](#1-build-and-run)
- [2. First example](#2-first-example)
- [3. Sockets and ownership](#3-sockets-and-ownership)
- [4. TCP byte operations](#4-tcp-byte-operations)
- [5. TCP messages](#5-tcp-messages)
- [6. UDP datagrams](#6-udp-datagrams)
- [7. Addresses and IPv6](#7-addresses-and-ipv6)
- [8. Timeouts](#8-timeouts)
- [9. Cancellation](#9-cancellation)
- [10. Closing each TCP direction](#10-closing-each-tcp-direction)
- [11. Errors and interrupted transfers](#11-errors-and-interrupted-transfers)
- [12. Public data types](#12-public-data-types)
- [13. Socket API reference](#13-socket-api-reference)
- [14. Compatibility and implementation](#14-compatibility-and-implementation)
- [15. License](#15-license)

## 1. Build and run

To use NCL, copy `NCL.hpp` into your project and include it:

```cpp
#include "NCL.hpp"
```

There is no separate NCL library to build. On Windows, link the operating
system's Winsock library. NCL initializes and cleans up Winsock automatically.
The commands below build the two example programs from this folder. Compile
them separately: each has its own `main()` function.

### Windows with GCC or MinGW

In PowerShell:

```powershell
g++ -std=c++17 -Wall -Wextra -Wpedantic server_example.cpp -o server_example.exe -lws2_32
g++ -std=c++17 -Wall -Wextra -Wpedantic client_example.cpp -o client_example.exe -lws2_32
```

Keep `-lws2_32` after the source or object files. Include `NCL.hpp` before an
optional `windows.h` to avoid conflicts with the older `winsock.h` header.

### Windows with MSVC

In a Visual Studio developer command prompt:

```bat
cl /EHsc /std:c++17 /W4 /utf-8 server_example.cpp /Fe:server_example.exe
cl /EHsc /std:c++17 /W4 /utf-8 client_example.cpp /Fe:client_example.exe
```

The header tells MSVC to link `Ws2_32.lib` automatically.

### Linux or Ubuntu in WSL

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic server_example.cpp -o server_example
g++ -std=c++17 -Wall -Wextra -Wpedantic client_example.cpp -o client_example
```

The included examples do not create threads. Add `-pthread` when compiling
application examples that use `std::thread`, such as a separate cancellation
controller.

Use the exact uppercase filename `NCL.hpp`, including on filesystems where
uppercase and lowercase names are different.

## 2. First example

The example uses two programs and a fixed local address and port:

- `server_example.cpp` opens a TCP server on `127.0.0.1:5000`, waits for one client,
  receives its message and replies.
- `client_example.cpp` connects to that server, sends a message and reads the reply.

There are no command-line arguments. After compiling, start the server in
one terminal, then the client in another. On Windows:

```powershell
# Terminal 1
.\server_example.exe
# Terminal 2
.\client_example.exe
```

On Linux, run `./server_example` and `./client_example`. The server prints
`Client: Hello from the client!`; the client prints
`Server: Hello from the server!`. Both exit after this exchange.

These first examples use the default blocking calls: the server waits for a
connection, and each receive waits for data or EOF. Stop a waiting program
from its terminal when needed. Timeout options are covered in section 8.

This is the complete server in `server_example.cpp`:

```cpp
#include "NCL.hpp"
#include <iostream>

int main() {
    try {
        // 1. Open a TCP server on this computer, port 5000.
        ncl::ServerSocket server(5000, "127.0.0.1");
        std::cout << "Waiting for a client..." << std::endl;

        // 2. Wait for a client to connect.
        ncl::Socket client = server.accept();

        // 3. Read one message and send a reply.
        auto message = client.receiveMessage();
        if (message) {
            std::cout << "Client: " << *message << '\n';
            client.sendMessage("Hello from the server!");
        }
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
```

`accept()` returns the socket for the connected client. `receiveMessage()`
returns an optional string: it contains the complete message, or is empty
when input ends before another message. `*message` accesses that string.
`sendMessage()` sends the reply. Leaving the scope closes the sockets.

The client uses the same message methods after connecting with
`ncl::Socket socket("127.0.0.1", 5000)`. See [client_example.cpp](client_example.cpp) for its
complete source. UDP is covered separately in section 6.

### Trying a local network

Find the server computer's address with `ipconfig` on Windows or `ip -4 addr`
on Linux. For example, if the server is at `192.168.1.50`:

1. In `server_example.cpp`, change the server binding from `"127.0.0.1"` to `"0.0.0.0"`.
2. In `client_example.cpp`, change the destination to `"192.168.1.50"`, using the real address.
3. Recompile both programs, then run the server and client on their respective computers.

Allow the application or TCP port 5000 through the server's firewall for the
private network. Guest Wi-Fi networks may prevent devices from reaching one another.

`0.0.0.0` is a binding address for all local IPv4 interfaces, not a destination.
For IPv6, use `::` as the server binding and its real IPv6 address as the
destination. `127.0.0.1` and `::1` never reach another computer.

## 3. Sockets and ownership

TCP has two roles. `ServerSocket` owns a listening socket. Each successful
`accept()` creates a new `Socket` for one client. The listener remains open
and can accept another connection. Closing the listener does not close the
already accepted sockets.

```cpp
ncl::ServerSocket server(5000, "127.0.0.1");
auto client = server.accept();
```

A `Socket` constructed with no arguments is empty. Call `connect()` later, or
construct it directly with a host and port. `DatagramSocket` without arguments
is already bound to an available IPv4 port.

Sockets use RAII: leaving their scope releases their native resources, also
when an exception is thrown. Explicit `close()` is idempotent and does not
throw. Copying is forbidden; `std::move` transfers ownership and leaves the
source closed. Move assignment first releases the destination's old socket.

A closed `Socket` can reconnect. Replace a closed `ServerSocket` or
`DatagramSocket` with a new object. Other operations that require an open
socket report `InvalidState` when it is closed.

Use each socket object from one thread at a time. NCL does not synchronize
concurrent sends, receives, shutdown, move or `close()` on the same object.
Different socket objects can be used by different threads.

## 4. TCP byte operations

TCP carries a continuous stream of bytes. It preserves their order within a
connection, but not the boundaries of application writes. Sending `"Hello"`
and `"World"` separately does not tell the receiver that they were two messages.

```cpp
std::string text = "Hello";
socket.sendAll(text.data(), text.size());
```

`data()` gives the location of the bytes; `size()` gives their count. No extra
zero terminator is sent. Embedded zero bytes are included when they are part
of the specified length. Do not use `sizeof(pointer)` to measure its data.

| Member | Result |
| --- | --- |
| `sendSome(data, size, options = {})` | Returns how many bytes the system accepted in this attempt; it may be fewer than requested. |
| `sendAll(data, size, options = {})` | Continues from the correct position until all bytes are accepted or an error occurs. |
| `receive(buffer, capacity, options = {})` | Returns 1..capacity bytes, possibly fewer than requested; 0 means input has ended. |
| `receiveExact(buffer, size, options = {})` | Returns true after reading the requested count, or false for EOF before the first byte. |

`receive()` needs positive capacity. A non-empty buffer must not be null.
Zero-length sends do no I/O on an open output; a zero-length exact read
succeeds without reading. Wait options are still validated.

EOF after a partially completed exact read reports `UnexpectedEof` and closes
the socket. A clean EOF before any bytes leaves it open, since output may
still be usable. Locally shutting input also produces EOF for later reads.

A successful send means the system accepted the bytes for transmission. It
does not prove that the remote program read them; use an application reply
when that confirmation matters. Do not serialize a C++ object by sending its
raw memory: pointers, padding and number formats are not a shared protocol.

## 5. TCP messages

`sendMessage()` and `receiveMessage()` provide an optional framing format over
the same TCP stream:

```cpp
socket.sendMessage("Hello", 4096, wait);
auto message = socket.receiveMessage(4096, wait);
```

Each message contains:

1. A 4-byte unsigned length in **big-endian** order, most significant byte first.
2. Exactly that many content bytes, with no added terminator.

```text
"Hello" -> 00 00 00 05 48 65 6C 6C 6F
           length      content
```

The helper gathers partial reads and writes and consumes only the current
message, even when several arrived together. The default content limit is
`ncl::defaultMaxMessageSize`: **1 MiB, or 1,048,576 bytes**. The incoming length
is checked before allocating the content. Set another maximum through the
`maxBytes` argument; the wire length remains limited to `UINT32_MAX`.

The limit is in bytes, not Unicode characters. Content can contain UTF-8,
newlines and zero bytes. NCL does not convert text encodings. Limit 0 accepts
only empty messages. An empty string inside the optional is a valid message;
`std::nullopt` means input ended before the next prefix.

An oversized outgoing message is rejected before sending. An oversized
incoming message or EOF inside a frame closes the socket. Interrupted-frame
rules are described in [section 11](#11-errors-and-interrupted-transfers).

Both programs must use the same format. This is different from Java
`println()` / `readLine()` or `writeUTF()`. A Java peer can use
`DataInputStream.readInt()` with `readFully()`, and `DataOutputStream.writeInt()`
with `write()`, while checking sizes and agreeing on the content encoding.
Do not mix raw bytes and framed messages without defining their exact order.

## 6. UDP datagrams

UDP exchanges separate datagrams. It has no `accept()` and does not use the
TCP length prefix. Each received packet includes the sender's endpoint.

```cpp
ncl::DatagramSocket socket; // Available local IPv4 port.
socket.send({"Hello", "127.0.0.1", 6000}, wait);
auto packet = socket.receive(2048, wait);
```

UDP does not guarantee arrival, ordering or absence of duplicates. A successful
send does not prove delivery. An empty datagram is valid and does not mean EOF.

`receive(capacity, options)` consumes one packet. Capacity must be between 1
and `ncl::maxDatagramSize`, **65507 bytes**. If the packet does not fit, NCL
discards it and reports `DatagramTruncated`. It never returns a truncated
prefix as a complete packet. The next receive reads a new datagram.

The same conservative 65507-byte content cap applies to IPv4 and IPv6. NCL
does not implement IPv6 jumbograms. Networks may impose smaller limits and
large packets may require IP fragmentation; use small datagrams for initial
tests. NCL does not split one application datagram into several packets.

To reply, preserve the address, family and scope from reception:

```cpp
auto packet = socket.receive(2048, wait);
packet.data = "Reply";
socket.send(packet, wait);
```

## 7. Addresses and IPv6

| Address | Use |
| --- | --- |
| `127.0.0.1` | IPv4 loopback: this computer. |
| `::1` | IPv6 loopback: this computer. |
| `0.0.0.0` | Bind to all local IPv4 interfaces. |
| `::` | Bind to all local IPv6 interfaces. |

NCL accepts numeric addresses and hostnames. A TCP client defaults to
`AddressFamily::Unspecified`, trying IPv4/IPv6 addresses in resolver order.
Filter to one family when needed:

```cpp
ncl::Socket socket("localhost", 5000, wait, ncl::AddressFamily::IPv6);
```

For servers and UDP, the binding address selects the socket family:

```cpp
ncl::ServerSocket server(5000, "::1");
ncl::DatagramSocket udp(6000, "::1");
```

The default binding remains IPv4 `0.0.0.0`. With a binding hostname, NCL uses
the first address it can bind; a numeric address makes the choice explicit.

### IPv6-only and dual stack

An IPv6 binding defaults to `IPv6Mode::IPv6Only`. To accept IPv4 and IPv6 on
the same port, bind `::` and explicitly request dual stack:

```cpp
ncl::ServerSocket server(5000, "::", 16, ncl::IPv6Mode::DualStack);
ncl::DatagramSocket udp(6000, "::", ncl::IPv6Mode::DualStack);
```

NCL sets `IPV6_V6ONLY` before binding on both systems. A separate IPv4 socket
can share a port with an IPv6-only binding; a dual-stack binding uses the port
for both families. Binding `::1` remains specific to IPv6 loopback even in
dual-stack mode. Requesting dual stack on an IPv4 binding is invalid.

The system may represent an IPv4 peer on an IPv6 socket as an IPv4-mapped
address, such as `::ffff:127.0.0.1`. NCL presents returned endpoints as ordinary
IPv4 addresses and converts them back when sending from a dual-stack UDP
socket. IPv6-only UDP sockets do not accept mapped IPv4 destinations.

UDP resolves destinations compatible with its binding and the optional
`packet.family` filter. An ordinary IPv4 socket does not switch to IPv6 for
one send. Use a binding such as `DatagramSocket(0, "::")` for IPv6.

To try the first TCP example on IPv6 loopback, replace `"127.0.0.1"` with
`"::1"` in both source files and recompile. For UDP, choose an IPv6 binding
explicitly before sending to an IPv6 address or hostname.

### Scope IDs

IPv6 link-local addresses may need an interface index, for example
`fe80::1234%7`. The number is an example: use the actual index of the interface.
Numeric zones are the portable form; interface names depend on the system.

NCL preserves a received scope in `scopeId` and as `%number` in the address.
For UDP output, either the address zone or `packet.scopeId` can supply it. If
both are nonzero, they must agree. IPv4 destinations do not use IPv6 scope IDs.
TCP takes a zone as part of the host string.

Endpoint display can use `[::1]:5000`, but pass `::1` without brackets when
the API takes the host and port separately. Dual stack does not create IPv6
connectivity if the network or operating system does not provide it.

## 8. Timeouts

Every waiting operation accepts an optional `WaitOptions`:

```cpp
ncl::WaitOptions wait;
wait.timeout = std::chrono::seconds(5);
auto message = socket.receiveMessage(4096, wait);
```

| Timeout | Meaning |
| --- | --- |
| Zero milliseconds | No NCL deadline. |
| A positive duration | Maximum waiting budget for this operation. |
| A negative duration | Invalid; reports `InvalidArgument`. |

The timeout applies to `connect`, `accept`, TCP byte/message transfers and UDP
send/receive. `sendAll`, `receiveExact` and message helpers share **one
deadline** across all their parts. It does not restart after each read, write
or after the message prefix. Reusing options in another call starts a new
budget for that call.

Timeouts use a monotonic clock. Scheduling delays can postpone when the
application observes expiry; NCL does not provide real-time timing guarantees.

### DNS is outside the budget

Portable `getaddrinfo()` resolution remains synchronous and cannot be
interrupted by NCL. For connection and UDP sending, the timeout starts after
resolution. Cancellation is checked before and after it, so a request made
during DNS is observed only when resolution returns.

One TCP budget covers the sequential attempts over the resolved addresses.
An unresponsive first address may consume it before another family is tried.
Use a numeric address to avoid hostname lookup, or select a family explicitly
when that matches your application.

## 9. Cancellation

A cancellation token lets another thread request an operation to stop without
concurrently changing or closing its socket. Create a source and pass its
token in the wait options:

```cpp
ncl::CancellationSource stop;
ncl::WaitOptions wait{std::chrono::seconds(10), stop.token()};

// On the thread that owns the socket:
auto packet = socket.receive(2048, wait);
```

When a separate controller thread needs to stop that wait, it calls:

```cpp
stop.requestCancellation();
```

The waiting call reports `Cancelled`. Only the request is concurrent: wait
for the owner thread to finish, such as with `join()`, before closing or
moving its socket. Do not call `close()` or shutdown from a second thread
while an operation is in progress.

Source copies share one request. `requestCancellation()` returns true only
for the first request. Tokens retain their state after a source is destroyed.
A request cannot be reset; create a new source or use an empty token for a
new set of operations. A pre-cancelled token rejects the operation before I/O.

During socket waits, NCL checks a token at intervals of up to 20 ms, plus
scheduling delays. This does not apply during synchronous DNS. Completion can
win a race with cancellation; a request does not undo bytes already accepted
or a datagram already consumed.

## 10. Closing each TCP direction

TCP has independent input and output directions:

```cpp
socket.sendMessage("Request", 4096, wait);
socket.shutdownOutput();
auto reply = socket.receiveMessage(4096, wait);
```

`shutdownOutput()` disallows further sends and lets the peer read the queued
bytes followed by EOF. It keeps local input available for a reply. Later sends
report `InvalidState`.

`shutdownInput()` stops exposing input. Later reads report EOF: `receive()`
returns 0, `receiveExact()` false for a positive size, and `receiveMessage()`
`nullopt`. It does not locally shut output. On Windows, unread data or later
arrivals after input shutdown can cause a native connection reset.

Both operations are idempotent while the socket is open. Neither releases
the native handle; destruction or `close()` does that. Neither is a supported
way to interrupt another thread's active operation. UDP has no half-close API.

## 11. Errors and interrupted transfers

NCL reports its failures through `ncl::NetworkError`, derived from
`std::runtime_error`:

```cpp
try {
    auto message = socket.receiveMessage(4096, wait);
    // Handle a message or EOF.
} catch (const ncl::NetworkError& error) {
    std::cerr << error.what() << '\n';
    if (error.kind() == ncl::ErrorKind::Timeout) {
        // Check isOpen() before deciding whether to retry.
    }
}
```

| Diagnostic | Meaning |
| --- | --- |
| `what()` | Readable operation and error description. |
| `kind()` | Portable `ErrorKind` category. |
| `nativeCode()` | Winsock/errno for `System`, resolver code for `Resolution`, or 0 when no native code applies. |
| `bytesTransferred()` | Bytes transferred by the failing TCP call; includes framing bytes for message helpers. |

The categories are `System`, `Resolution`, `InvalidArgument`, `InvalidState`,
`MessageTooLarge`, `UnexpectedEof`, `DatagramTruncated`, `Timeout` and `Cancelled`.
`Timeout` identifies NCL's configured deadline. A timing failure reported by
the operating system can instead be `System` with its native code.

| Situation | Socket state afterward |
| --- | --- |
| TCP timeout/cancellation before any transferred byte | Open; retrying with fresh options is possible. |
| TCP timeout/cancellation after any transferred byte in that call | Closed; the operation was incomplete. |
| TCP native transfer error or partial-read EOF | Closed. |
| Clean TCP EOF before any read byte | Open, with input ended. |
| Oversized outgoing message or invalid arguments before I/O | Unchanged. |
| Oversized incoming frame | Closed after its prefix. |
| `accept()` timeout/cancellation | Listener remains open. |
| UDP timeout/cancellation or truncation | UDP socket remains open. |
| Failed, timed-out or cancelled connection attempt | No new owned socket remains open. |

Do not automatically resend an entire failed message: some of it may already
have arrived. NCL closes partially transferred operations to avoid continuing
on a stream whose application boundaries are no longer known. On 32-bit
targets, an unrepresentable progress count saturates at `SIZE_MAX`.

Standard exceptions, such as `std::bad_alloc`, retain their type. NCL's error
text is in English; native error text can use the operating system's language.

## 12. Public data types

| Type | Public fields or values |
| --- | --- |
| `ncl::Endpoint` | `std::string address`, `std::uint16_t port`, `AddressFamily family`, `std::uint32_t scopeId`. |
| `ncl::DatagramPacket` | `std::string data`, then the same address, port, family and scope fields. |
| `ncl::WaitOptions` | `std::chrono::milliseconds timeout{0}`, `CancellationToken cancellation{}`. |
| `ncl::AddressFamily` | `Unspecified`, `IPv4`, `IPv6`. |
| `ncl::IPv6Mode` | `IPv6Only`, `DualStack`. |
| `ncl::CancellationSource` | `token()`, `requestCancellation()`. Copies share the request. |
| `ncl::CancellationToken` | `canBeCancelled()`, `isCancellationRequested()`. A default token has no source. |

Ports default to 0, scope IDs to 0 and families to `Unspecified`. Validate
external port numbers before converting to `uint16_t`. Port 0 is allowed for
a local binding, where the system chooses a free port; it is rejected as a
destination. Discover the actual binding with `localEndpoint()`.

Received endpoints identify IPv4 or IPv6 and use numeric address strings.
`DatagramPacket::data` is binary-safe. No structure should be transmitted by
copying its raw C++ object memory.

## 13. Socket API reference

Tables use shortened parameter names. All wait options default to `{}`; a
message's `maxBytes` defaults to `ncl::defaultMaxMessageSize`. Code should use
the `ncl::` prefix when naming public types.

### `ncl::Socket`

| Constructor or member | Purpose |
| --- | --- |
| `Socket()` | Creates an empty object; `isOpen()` is false. |
| `Socket(host, port, options = {}, family = AddressFamily::Unspecified)` | Resolves and connects to a peer. |
| `connect(host, port, options = {}, family = AddressFamily::Unspecified)` | Connects an empty or closed socket. |
| `sendSome`, `sendAll`, `receive`, `receiveExact` | Byte operations from section 4. |
| `sendMessage(message, maxBytes, options)` | Sends a length-prefixed message. |
| `receiveMessage(maxBytes, options)` | Returns `optional<string>` for a complete message or EOF. |
| `localEndpoint()`, `remoteEndpoint()` | Returns the local or peer endpoint. |
| `isOpen()`, `isConnected()` | Reports local owned/established state, not a remote liveness probe. |
| `hasReachedEof()` | Reports an observed remote EOF; local input shutdown alone does not set it. |
| `shutdownInput()`, `shutdownOutput()` | Shuts one direction without releasing the socket. |
| `isInputShutdown()`, `isOutputShutdown()` | Reports locally requested shutdown states. |
| `close()` | Releases the handle and resets observed states; idempotent and noexcept. |

### `ncl::ServerSocket`

```cpp
ServerSocket(port, bindAddress = "0.0.0.0", backlog = 16,
             mode = IPv6Mode::IPv6Only);
```

`backlog` requests the pending-connection queue size, subject to system limits.
It is not a lifetime limit on the number of clients.

| Member | Purpose |
| --- | --- |
| `accept(options = {})` | Waits for a client and returns a new `Socket`. |
| `localEndpoint()` | Returns the bound numeric address, family and actual port. |
| `isOpen()` | Reports whether the listener is owned and open. |
| `close()` | Releases only the listener; accepted sockets remain independent. |

### `ncl::DatagramSocket`

```cpp
DatagramSocket(port = 0, bindAddress = "0.0.0.0",
               mode = IPv6Mode::IPv6Only);
```

| Member | Purpose |
| --- | --- |
| `send(packet, options = {})` | Sends one datagram to the specified destination. |
| `receive(capacity = maxDatagramSize, options = {})` | Returns one complete packet with its sender endpoint, or reports truncation. |
| `localEndpoint()` | Returns the bound endpoint. |
| `isOpen()` | Reports whether the socket is owned and open. |
| `close()` | Releases the bound socket. |

## 14. Compatibility and implementation

### Supported environments

| Environment | Support |
| --- | --- |
| Windows | Native Winsock, with `ws2_32` linked. |
| Linux, including Ubuntu in WSL | Native Linux socket APIs. |
| IPv4 and IPv6 | Numeric addresses and hostnames, with the documented family/binding rules. |
| macOS, BSD and other systems | Not supported; compilation reports an error. |

Socket methods are synchronous from the caller's perspective. Internally NCL
uses nonblocking handles with `select` on Windows and `poll` on Linux to manage
deadlines and cancellation. TCP native attempts are limited to 64 KiB so long
transfers check options regularly. NCL does not create internal worker threads.

The definitions are inline or inside classes, so the header can be included
in multiple source files. `ncl::detail` is implementation code, not part of
the application API. The public message format is unchanged from earlier
source versions.

### Scope of checks

Development checks included GCC/MinGW and MSVC on Windows x64, and GCC on
Ubuntu x64 in WSL, with AddressSanitizer and UndefinedBehaviorSanitizer on
Linux. They covered TCP/UDP loopback exchanges, binary and empty data, framing,
limits, timeouts, cancellation, resets, ownership, IPv6-only and dual-stack
behavior. Scope handling also has synthetic address checks.

These checks do not cover every network, architecture or compiler. Two physical
computers on a LAN, real link-local interface exchanges, Linux outside WSL,
32-bit targets, induced UDP loss/reordering and prolonged load are not declared
verified. DNS resolution remains synchronous and outside socket timeouts.

HTTP, TLS, multicast, IPv6 jumbograms and an asynchronous I/O API are outside
this version. NCL transfers application bytes without providing encryption.
This folder is the minimal source distribution; it does not include the
development test suite or generated build files.

## 15. License

NCL is released under the [MIT License](LICENSE).

Artificial intelligence was used to help create this project, including its code
and documentation.

Copyright © 2026 allekks_b.
