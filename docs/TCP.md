# NCL TCP guide

![Protocol Guide](../assets/docs/protocol.svg)

[Home](../README.md) | [Start here](GETTING_STARTED.md) | [Server tutorial](SERVER.md) | [API reference](API.md) | [Build](BUILDING.md)

This guide explains how to create TCP servers and clients with NCL, exchange
messages or raw bytes, stop waiting operations and handle connection failures.
All public names below belong to `ncl`; `ncl::detail` is internal implementation.
You need C++17 and [NCL.hpp](../NCL.hpp). No third-party library is required.

Save each complete **File** program next to `NCL.hpp` in your application folder;
run build commands there. The `docs/` directory contains the guides, not the
working directory for compilation. [Build instructions](BUILDING.md).

## Contents

- [Understand the connection](#understand-the-connection)
- [Prepare the project](#prepare-the-project)
- [First server and client](#first-server-and-client)
- [Compile and run the pair](#compile-and-run-the-pair)
- [Exchange multiple messages and clients](#exchange-multiple-messages-and-clients)
- [Understand NCL message framing](#understand-ncl-message-framing)
- [Work with raw bytes](#work-with-raw-bytes)
- [Addresses, ports, IPv6 and LAN use](#addresses-ports-ipv6-and-lan-use)
- [Timeouts and a shared operation budget](#timeouts-and-a-shared-operation-budget)
- [Cancel a wait safely](#cancel-a-wait-safely)
- [EOF, half-close and connection state](#eof-half-close-and-connection-state)
- [Ownership, movement and reconnection](#ownership-movement-and-reconnection)
- [Errors, progress and retry decisions](#errors-progress-and-retry-decisions)
- [API reference](#api-reference)
- [Troubleshoot a TCP program](#troubleshoot-a-tcp-program)

## Understand the connection

A TCP server first creates a **listener** on a local address and port. A client
connects to that address. The server accepts the connection and receives a new
socket used to communicate with that particular client.

For example, a `ServerSocket` can listen on port 5000. A client `Socket`
connects to that port, and `accept()` returns the server's connected `Socket`.
The client and this accepted socket can then exchange data in both directions.

The listening socket and the connected socket have different jobs. Data is
sent through `Socket`, never through `ServerSocket`. Closing the listener
prevents new connections; already accepted connections remain independent.

> [!NOTE]
> TCP delivers an ordered stream of bytes. It does not preserve your calls as
> messages: two writes can arrive together, and one write can require many reads.
> Choose a shared framing format before writing your application. NCL offers
> length-prefixed messages, or byte operations for protocols you define yourself.

For ordinary web endpoints, use [HTTP](HTTP.md), whose message format is already
defined. NCL's TCP message helpers do not produce HTTP messages.

## Prepare the project

Copy `NCL.hpp` next to your source and include it with its exact uppercase name:

```cpp
#include "NCL.hpp"
```

The snippets that follow an explanation of an existing variable are fragments
to place inside your program. Blocks explicitly labelled **File** are complete
programs: save each one under its stated filename and compile it separately.
Two files with `main()` cannot be linked into the same executable.

Windows needs Winsock. GCC/MinGW uses `-lws2_32`; MSVC links it automatically
through the header. NCL performs Winsock initialization and cleanup itself.
Include NCL before an optional `windows.h` to avoid old Winsock header conflicts.

## First server and client

Start with this one-message exchange. Both programs use `127.0.0.1:5000`, so
they run on the same computer. Neither program creates application threads.

### Complete server

**File: `server_example.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>

int main() {
    try {
        // 1. Open a TCP server on this computer, port 5000.
        ncl::ServerSocket server(5000, "127.0.0.1");
        std::cout<<"Waiting for a client..."<<std::endl;

        // 2. Wait for a client to connect.
        ncl::Socket client = server.accept();

        // 3. Read one message and send a reply.
        auto message = client.receiveMessage();
        if (message) {
            std::cout<<"Client: "<<*message<<'\n';
            client.sendMessage("Hello from the server!");
        }
    } catch (const std::exception& error) {
        std::cerr<<"Error: "<<error.what()<<'\n';
        return 1;
    }
}
```

Read the server in order:

1. `ServerSocket(5000, "127.0.0.1")` binds and starts listening. Port 5000 must
   be available for TCP on that local address.
2. `accept()` waits for a client and returns the connected `Socket`. The
   listener remains open and could accept another client later.
3. `receiveMessage()` waits for a complete NCL-framed message or clean EOF.
4. `if (message)` checks whether a message exists. It is not a test for whether
   the text contains any characters: an empty string is still a valid message.
5. `*message` accesses the received string; `sendMessage()` sends the reply.
6. Leaving the scope closes the owned sockets, including when an exception
   leaves the `try` block.

### Complete client

**File: `client_example.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>

int main() {
    try {
        // 1. Connect to the server on this computer, port 5000.
        ncl::Socket socket("127.0.0.1", 5000);

        // 2. Send one message.
        socket.sendMessage("Hello from the client!");

        // 3. Read the server's reply.
        auto reply = socket.receiveMessage();
        if (reply) {
            std::cout<<"Server: "<<*reply<<'\n';
        }
    } catch (const std::exception& error) {
        std::cerr<<"Error: "<<error.what()<<'\n';
        return 1;
    }
}
```

The client constructor connects immediately. It then sends a message, waits
for the reply and exits. The connected sockets permit communication in both
directions; neither side is permanently restricted to sending or receiving.

These introductory calls have no configured timeout. A server with no client
continues waiting in `accept()`. A peer that connects but sends nothing can
leave `receiveMessage()` waiting. Use the bounded examples later when a wait
must end automatically.

## Compile and run the pair

Run commands from the directory containing the saved sources and `NCL.hpp`.

### Windows with GCC or MinGW

```powershell
g++ -std=c++17 -Wall -Wextra -Wpedantic server_example.cpp -o server_example.exe -lws2_32
g++ -std=c++17 -Wall -Wextra -Wpedantic client_example.cpp -o client_example.exe -lws2_32
```

Keep `-lws2_32` after sources/object files. Open two terminals in that directory.
In terminal 1:

```powershell
.\server_example.exe
```

In terminal 2:

```powershell
.\client_example.exe
```

The server prints `Client: Hello from the client!`; the client prints
`Server: Hello from the server!`. Both terminate after that exchange. Stop a
waiting server with Ctrl+C before recompiling its executable on Windows.

### Windows with MSVC

Use a Visual Studio developer command prompt:

```bat
cl /EHsc /std:c++17 /W4 /utf-8 server_example.cpp /Fe:server_example.exe
cl /EHsc /std:c++17 /W4 /utf-8 client_example.cpp /Fe:client_example.exe
```

Start the resulting executables in separate terminals as above.

### Linux or WSL

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic server_example.cpp -o server_example
g++ -std=c++17 -Wall -Wextra -Wpedantic client_example.cpp -o client_example
```

Run `./server_example`, then `./client_example` in a second terminal. Add
`-pthread` for programs that use `std::thread`, including the cancellation
example in this guide. Use a filesystem with the exact `NCL.hpp` spelling.

## Exchange multiple messages and clients

The first server handles one connection and one message because its application
code does that. Neither TCP nor NCL imposes that lifetime limit.

For several messages on an already accepted socket, place the receive/reply
inside a loop. This fragment assumes an existing `ncl::Socket client`:

```cpp
ncl::WaitOptions wait{std::chrono::seconds(10)};
while (auto message = client.receiveMessage(4096, wait)) {
    client.sendMessage("Echo: " + *message, 8192, wait);
}
```

The loop ends when input reaches clean EOF before another frame. A partial
frame, timeout or other failure throws instead. Every call above receives a
fresh budget; the ten seconds are not a lifetime limit on the connection.

To handle clients one after another with bounded message waits and an
unlimited wait for the next connection:

**File: `tcp_loop_server.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>

int main() {
    try {
        ncl::ServerSocket listener(5001, "127.0.0.1");
        ncl::WaitOptions wait{std::chrono::seconds(10)};
        std::cout<<"Listening on 127.0.0.1:5001"<<std::endl;
        while (true) {
            auto client = listener.accept();
            try {
                while (auto message = client.receiveMessage(4096, wait)) {
                    client.sendMessage("Echo: " + *message, 8192, wait);
                }
            } catch (const ncl::NetworkError& error) {
                std::cerr<<"Client ended: "<<error.what()<<'\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
```

**File: `tcp_loop_client.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>

int main() {
    try {
        ncl::WaitOptions wait{std::chrono::seconds(5)};
        ncl::Socket socket("127.0.0.1", 5001, wait);
        for (const auto* text : {"first", "second", "third"}) {
            socket.sendMessage(text, 4096, wait);
            const auto reply = socket.receiveMessage(8192, wait);
            if (!reply) throw std::runtime_error("Server ended before replying");
            std::cout<<*reply<<'\n';
        }
        socket.shutdownOutput();
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
```

Compile these filenames with the same platform commands, replacing the source
and executable names. The client prints three replies. The server returns to
`accept()` after the client finishes; use Ctrl+C to stop it.

This loop serves one client at a time. For simultaneous clients, an application
can move separate accepted sockets to worker threads and define its own worker
limits and shutdown policy. NCL does not create or manage those threads. The
listener should still have one owner, and each connection should have one owner.

## Understand NCL message framing

`sendMessage` writes a four-byte unsigned big-endian length followed by exactly
that many body bytes. It adds no text terminator, newline or encoding conversion.

For `"Hello"`, the length prefix is `00 00 00 05` and the content is
`48 65 6C 6C 6F`, in hexadecimal. The complete frame occupies nine bytes.

`receiveMessage` gathers partial native reads and consumes only the current
frame, even if several frames arrived together. It returns
`std::optional<std::string>`:

| Result | Meaning |
| --- | --- |
| Optional containing `"Hello"` | A complete message was read. |
| Optional containing `""` | A complete zero-length message was read. |
| `std::nullopt` | Clean EOF before the next prefix began. |
| Exception | Invalid/incomplete frame, interrupted transfer or another failure. |

The default body cap is `ncl::defaultMaxMessageSize`, 1,048,576 bytes (1 MiB).
Set `maxBytes` independently on each call. A limit of zero accepts only empty
messages. Lengths count bytes, not Unicode characters; UTF-8 and embedded zero
bytes are preserved. The wire length cannot exceed `UINT32_MAX` even if you
choose a larger application cap.

An oversized outgoing message is rejected before sending, leaving the socket
usable. An oversized incoming length is checked before allocating its body;
NCL closes that connection because the rejected frame remains in the stream.
EOF inside the prefix or body also reports an error and closes the socket.

Both peers must implement this format. It differs from newline-delimited text,
Java `println`/`readLine`, Java `writeUTF`, HTTP and WebSocket. A Java peer can
use `DataOutputStream.writeInt` plus `write`, and `DataInputStream.readInt`
plus `readFully`, with limits and a shared text encoding. Treat the four wire
bytes as an unsigned length when validating their allowed range.

## Work with raw bytes

Use raw byte operations when a protocol already defines its own format or
when you are designing a different one. Do not mix framed messages and raw
bytes unless both peers agree on the precise sequence.

### Send some bytes or all bytes

For an existing connected `socket`:

```cpp
std::string data = "Hello";
ncl::WaitOptions wait{std::chrono::seconds(5)};
socket.sendAll(data.data(), data.size(), wait);
```

`data()` identifies the bytes and `size()` counts them. Do not use
`sizeof(pointer)` as the length of pointed-to data. A successful send means
the operating system accepted bytes for transmission, not that the remote
application processed them. Define an application reply when that matters.

`sendSome(data, size, options)` performs one successful native transfer and
returns its byte count. That count can be smaller than requested. If you need
the complete buffer, use `sendAll`, which advances through the buffer itself.
Native attempts are capped at 64 KiB so long operations check wait options.

### Receive available data or an exact count

`receive(buffer, capacity, options)` returns between 1 and `capacity` bytes,
possibly less than requested, or zero for EOF. It requires positive capacity.
The returned bytes are not automatically null-terminated. This fragment
prints only the bytes actually received:

```cpp
char buffer[4096];
const auto count = socket.receive(buffer, sizeof(buffer), wait);
std::cout.write(buffer, static_cast<std::streamsize>(count));
```

Use `receiveExact` only when your protocol tells you the required byte count.
It returns true after reading that count, false for EOF before its first byte,
and throws `UnexpectedEof` if EOF occurs partway through it. That partial EOF
closes the socket. Choosing an arbitrary large exact count can leave you
waiting for bytes the peer never intended to send.

Non-empty buffers cannot be null. Zero-length sends do no network I/O on an
open output; zero-length exact reads succeed, including after EOF. They still
validate wait options. A closed socket or locally shut output is not made
valid by passing zero length.

### Agree on serialization

Write numeric values in a documented byte order and decide how strings and
collections are represented. Do not transmit the raw memory of an arbitrary
C++ object: pointers, padding, endianness and type layouts do not define a
portable protocol. NCL transfers bytes and does not serialize objects for you.

## Addresses, ports, IPv6 and LAN use

| Address | Meaning |
| --- | --- |
| `127.0.0.1` | IPv4 loopback, reachable on this computer. |
| `::1` | IPv6 loopback, reachable on this computer. |
| `0.0.0.0` | Bind to all local IPv4 interfaces. Not a client destination. |
| `::` | Bind to all local IPv6 interfaces; mode determines IPv4 acceptance. |

The default listener binding is `0.0.0.0`. These teaching programs explicitly
choose loopback. A port of zero is allowed for a local binding: the OS selects
a free port, found through `localEndpoint().port`. Destination port zero is
rejected. Validate external integers before converting to `std::uint16_t`;
conversion can discard an out-of-range number before NCL sees it.

`Socket` accepts numeric hosts and hostnames. By default it tries IPv4/IPv6
addresses sequentially in resolver order. Filter the family when required:

```cpp
ncl::Socket socket6("localhost", 5000, wait, ncl::AddressFamily::IPv6);
```

For IPv6 loopback, use `ServerSocket(5000, "::1")` and connect to `::1`.
NCL takes host and port separately here, so do not surround `::1` with brackets.
Brackets are used for display or inside [HTTP URLs](HTTP.md#urls-and-addresses).

An IPv6 listener is IPv6-only by default. For one port serving both families:

```cpp
ncl::ServerSocket both(5000, "::", 16, ncl::IPv6Mode::DualStack);
```

NCL explicitly sets `IPV6_V6ONLY` before binding. Dual stack requires an IPv6
binding; asking for it on an IPv4 binding is invalid. Binding `::1` remains
specific to IPv6 loopback. An IPv6-only listener can coexist with an IPv4
listener on the same port; a dual-stack binding occupies that port for both.
Mapped IPv4 peers such as `::ffff:127.0.0.1` are reported as ordinary IPv4
endpoints. Dual stack cannot create missing network connectivity.

For a LAN exchange, change the server bind address to `0.0.0.0` and the client
destination to the server computer's real IPv4 address, for example
`192.168.1.50`. Discover addresses with `ipconfig` on Windows or `ip -4 addr`
on Linux. Recompile and run the programs on their respective computers. Allow
the server application/port through its private-network firewall when needed.
Guest Wi-Fi can isolate devices. Loopback addresses never reach another computer.

IPv6 link-local destinations can need a numeric interface zone, such as
`fe80::1234%7`; use the real index, not this illustrative number. Numeric zones
are the portable form. Endpoint results preserve scope in `scopeId` and in
the address's `%number` suffix. Real link-local exchanges were not part of the
declared local verification.

## Timeouts and a shared operation budget

Waiting methods accept `WaitOptions`. Its fields are a duration in milliseconds
and an optional cancellation token:

```cpp
ncl::WaitOptions wait;
wait.timeout = std::chrono::seconds(5);
```

| Duration | Meaning |
| --- | --- |
| Zero | No NCL deadline. This is the default. |
| Positive | Maximum waiting budget for the operation. |
| Negative | Invalid; throws `InvalidArgument`. |

`connect`, `accept`, sends and receives all accept wait options. `sendAll`,
`receiveExact` and message helpers each share one deadline across every part
of that call. A message's payload does not receive a fresh deadline after its
prefix. Reusing `wait` in another call starts a new budget for that call.

Timeouts use a monotonic clock, with observation affected by scheduling.
They are not real-time guarantees. Portable hostname resolution remains
synchronous and cannot be interrupted by NCL: connection timing starts after
DNS, with cancellation checked before and after it. One TCP budget then covers
all resolved-address attempts. An unresponsive first address can consume it
before another family is tried.

## Cancel a wait safely

A `CancellationSource` owns shared cancellation state. Copying a source keeps
the same state; tokens remain valid after the source object is destroyed.
`requestCancellation()` returns true only for the first request. The request
cannot be reset: use a new source for another independent group of operations.

The socket has one owner. Another thread requests cancellation through the
source instead of closing, moving or shutting down that socket concurrently.
This complete example stops an otherwise idle `accept()`:

**File: `tcp_cancel.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>
#include <thread>

int main() {
    try {
        ncl::ServerSocket listener(0, "127.0.0.1");
        ncl::CancellationSource stop;
        ncl::WaitOptions wait{std::chrono::seconds(5), stop.token()};
        std::thread controller([stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.requestCancellation();
        });
        try {
            auto client = listener.accept(wait);
            std::cout<<"A client arrived before cancellation\n";
        } catch (const ncl::NetworkError& error) {
            controller.join();
            if (error.kind() != ncl::ErrorKind::Cancelled) throw;
            std::cout<<"Accept cancelled; listener still open: "<<listener.isOpen()<<'\n';
            return 0;
        } catch (...) {
            controller.join();
            throw;
        }
        controller.join();
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
```

Compile with `-pthread` when using GCC. Socket waits check a token at intervals
of up to 20 ms plus scheduling delays. Synchronous DNS is an exception. A
pre-cancelled token rejects an operation before I/O. Completion may win a race
with cancellation; requesting cancellation does not undo bytes already sent.

If the socket owner is a worker thread, request cancellation, `join()` that
worker, then close/move the socket. Different socket objects can be used by
different threads; NCL does not synchronize simultaneous operations on one object.

## EOF, half-close and connection state

TCP directions are independent. After an application finishes sending a
request it can call `shutdownOutput()`, which sends queued data followed by
EOF to its peer while keeping local input open for a response:

```cpp
socket.sendMessage("request", 4096, wait);
socket.shutdownOutput();
auto response = socket.receiveMessage(4096, wait);
```

Later sends fail with `InvalidState`. `shutdownInput()` makes later local reads
report EOF but does not locally shut output. On Windows, unread data or later
arrivals after input shutdown can produce a native connection reset. Avoid
using input shutdown as an application acknowledgment.

Both shutdown calls are idempotent while open. They do not release the handle
and are not supported ways to interrupt another thread's active operation.
Calling either on a closed socket reports `InvalidState`. A native shutdown
failure closes the entire socket and reports `System`.

| State query | What it actually tells you |
| --- | --- |
| `isOpen()` | A local handle is owned. |
| `isConnected()` | The local TCP socket is established/owned, equivalent to open here. |
| `hasReachedEof()` | A remote EOF has been observed by reading. |
| `isInputShutdown()` | Local input shutdown was requested. |
| `isOutputShutdown()` | Local output shutdown was requested. |

None is a remote liveness probe. A disconnected peer may only become visible
when you next perform I/O. Clean remote EOF leaves the local socket open
because output can still be useful. Local input shutdown alone does not set
`hasReachedEof`. `close()` releases the handle and resets all observed states.

## Ownership, movement and reconnection

Sockets own native resources through RAII. Destruction and `close()` release
them; explicit close is idempotent and does not throw. Socket objects cannot
be copied. Moving transfers ownership and leaves the source closed; move
assignment first releases the destination's previous resource.

```cpp
ncl::Socket connection;
connection.connect("127.0.0.1", 5000, wait);
ncl::Socket ownedElsewhere = std::move(connection);
```

After moving, operate on `ownedElsewhere`, not on the empty original. An empty
or closed `Socket` can connect again. A currently open one rejects a second
connect. Replace a closed `ServerSocket` with a new instance rather than trying
to bind the closed listener again. Returned endpoint objects are ordinary
values, not owners of network resources.

## Errors, progress and retry decisions

`NetworkError` derives from `std::runtime_error`:

| Member | Meaning |
| --- | --- |
| `what()` | Operation and readable error description. |
| `kind()` | Portable `ErrorKind`. |
| `nativeCode()` | Winsock/errno for System; resolver code for Resolution; otherwise zero. |
| `bytesTransferred()` | Bytes transferred by the failing TCP call, including message framing. |

Categories are `System`, `Resolution`, `InvalidArgument`, `InvalidState`,
`MessageTooLarge`, `UnexpectedEof`, `DatagramTruncated`, `Timeout`, `Cancelled`.
`DatagramTruncated` belongs to UDP. A native OS timing error may be `System`;
`Timeout` specifically identifies an NCL deadline.

| ErrorKind | Meaning |
| --- | --- |
| `System` | Native socket/runtime failure; inspect `nativeCode()`. |
| `Resolution` | Address lookup failed or found no compatible destination. |
| `InvalidArgument` | Invalid host, destination port, buffer, capacity, timeout or family/mode/scope argument. |
| `InvalidState` | Required socket is closed, output is shut down, or `connect` was called on an open socket. |
| `MessageTooLarge` | TCP body exceeds its configured/wire limit, or UDP payload exceeds its cap. |
| `UnexpectedEof` | Input ended inside an exact read or an NCL message frame. |
| `DatagramTruncated` | A UDP packet exceeded receive capacity and was discarded. |
| `Timeout` | The NCL operation deadline expired. |
| `Cancelled` | The supplied token requested cancellation. |

| Failure | Local socket afterward |
| --- | --- |
| TCP timeout/cancellation before any byte transferred in that call | Open; retry with fresh options is possible. |
| TCP timeout/cancellation after any byte transferred in that call | Closed. |
| Native transfer error or EOF after part of an exact read | Closed. |
| Clean EOF before a new read/frame | Open, input ended. |
| Outgoing message too large or invalid arguments before I/O | Unchanged. |
| Incoming message too large | Closed after its prefix. |
| Accept timeout/cancellation | Listener remains open. |
| Failed connection attempt | No new owned connection remains open. |

Never automatically resend a whole failed message: the peer may already have
received some or all of its content. Decide whether a retry is valid in your
application protocol, using IDs/acknowledgments if necessary. On 32-bit targets,
unrepresentable framing progress saturates at `SIZE_MAX`.

An error-handling fragment for an existing socket:

```cpp
try {
    auto reply = socket.receiveMessage(4096, wait);
    if (reply) std::cout<<*reply<<'\n';
} catch (const ncl::NetworkError& error) {
    std::cerr<<error.what()<<'\n';
    if (error.kind() == ncl::ErrorKind::Timeout && socket.isOpen()) {
        // No bytes were transferred by this call; a later fresh wait is possible.
    }
}
```

Standard exceptions such as `std::bad_alloc` retain their type. NCL diagnostics
are English, while native messages can follow the operating system's language.

## API reference

In the tables below, host and binding arguments are `const std::string&` and
ports are `std::uint16_t`. Buffer sizes, capacities and `maxBytes` are
`std::size_t`; `options` is a `WaitOptions` passed by value. Send buffers use
`const void*`, receive buffers use `void*`, and `message` is `std::string_view`.
Buffers/views must remain valid for the synchronous call; NCL does not retain
them afterward. Methods described as sending, closing, connecting or shutting
down return `void` unless a different result is stated.

### Socket

| Constructor or method | Parameters and result |
| --- | --- |
| `Socket()` | Empty, not open; noexcept. |
| `Socket(host, port, options = {}, family = AddressFamily::Unspecified)` | Resolve and connect. |
| `connect(host, port, options = {}, family = AddressFamily::Unspecified)` | Connect an empty/closed object. |
| `sendSome(data, size, options = {})` | Return `std::size_t`: bytes accepted by one native transfer, or 0 for a zero-size send. |
| `sendAll(data, size, options = {})` | Send the complete specified byte buffer, or throw. |
| `receive(buffer, capacity, options = {})` | Return `std::size_t`: bytes received, or zero for EOF. |
| `receiveExact(buffer, size, options = {})` | Return `bool`: true for the full count, false for EOF before any byte. |
| `sendMessage(message, maxBytes = defaultMaxMessageSize, options = {})` | Send the length-prefixed string view. |
| `receiveMessage(maxBytes = defaultMaxMessageSize, options = {})` | Return `std::optional<std::string>` for a complete message or `std::nullopt` for clean EOF. |
| `localEndpoint()`, `remoteEndpoint()` | Return an `Endpoint` value with numeric local/peer addressing; require an open socket. |
| `shutdownInput()`, `shutdownOutput()` | Close one direction without releasing the handle. |
| `isOpen()`, `isConnected()`, `hasReachedEof()` | Return `bool` for local ownership/observed connection state; noexcept. |
| `isInputShutdown()`, `isOutputShutdown()` | Return `bool` for locally requested direction states; noexcept. |
| `close()` | Release ownership and reset states; idempotent, noexcept. |

### ServerSocket

| Constructor or method | Parameters and result |
| --- | --- |
| `ServerSocket(port, bindAddress = "0.0.0.0", backlog = 16, mode = IPv6Mode::IPv6Only)` | Bind and listen; backlog must be positive. |
| `accept(options = {})` | Return one new connected `Socket`. |
| `localEndpoint()` | Return an `Endpoint` with binding and actual selected port; requires an open listener. |
| `isOpen()` | Return `bool` for listener ownership; noexcept. |
| `close()` | Release only the listener; idempotent, noexcept. |

Backlog requests the pending-connection queue size, subject to system limits.
Its type is `int`; mode is `IPv6Mode`. It is neither a lifetime client limit
nor a worker-thread count. Socket/listener destructors release ownership;
move construction and move assignment are noexcept, and copying is deleted.

### Shared value types

| Type | Fields or methods |
| --- | --- |
| `Endpoint` | In order: `std::string address`, `std::uint16_t port`, `AddressFamily family`, `std::uint32_t scopeId`. |
| `NetworkError(kind, message, nativeCode = 0, bytesTransferred = 0)` | Arguments: `ErrorKind`, `const std::string&`, `int`, `std::size_t`. Diagnostics are const/noexcept: `kind()` returns `ErrorKind`, `nativeCode()` returns `int`, `bytesTransferred()` returns `std::size_t`. Inherited `what()` returns `const char*`. |
| `WaitOptions` | In order: `std::chrono::milliseconds timeout{0}`, `CancellationToken cancellation{}`. |
| `CancellationSource` | Default construction creates shared state; const/noexcept `token()` returns `CancellationToken`; const/noexcept `requestCancellation()` returns `bool`. |
| `CancellationToken` | Const/noexcept `canBeCancelled()` and `isCancellationRequested()` return `bool`; default construction is noexcept and has no source. |
| `AddressFamily` | Unspecified, IPv4, IPv6. |
| `IPv6Mode` | IPv6Only, DualStack. |

An Endpoint defaults to an empty address, port/scope zero and Unspecified family;
endpoint values returned by successful socket queries identify the actual family.
The constant `defaultMaxMessageSize` has type `inline constexpr std::size_t`
and value 1,048,576. Endpoint, wait options, sources and tokens are copyable.
`AddressFamily` and `IPv6Mode` are scoped enums: use names such as
`ncl::AddressFamily::IPv6` and `ncl::IPv6Mode::DualStack`.

## Troubleshoot a TCP program

| Symptom | Check |
| --- | --- |
| Connection refused/System error | Start the server first; verify address, port and binding family. |
| Address already in use | Stop the previous listener or choose another port; do not start two copies on the same binding. |
| Server waits forever | It may be in accept or awaiting a message; add a timeout and log each stage. |
| Receive waits after a successful send | Confirm both peers use the same framing and that the peer actually sends a reply. |
| Empty message treated as disconnect | Test the optional, not whether the contained string is empty. |
| Local connection works but LAN fails | Replace loopback destination, bind a LAN interface, check firewall and Wi-Fi isolation. |
| Text contains unexpected zeroes/garbage | Use the actual byte count; do not print an unbounded C buffer or send pointer sizes. |
| Compiler reports multiple main definitions | Build complete examples as separate executables. |
| Include fails on Linux | Use the uppercase `NCL.hpp` filename. |

See the [README verification scope](../README.md#status-and-limits)
for tested environments and the limits of local checks. Continue with
[UDP](UDP.md) for independent packets or [HTTP](HTTP.md) for web requests and APIs.
