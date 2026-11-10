# NCL UDP guide

![Protocol Guide](../assets/docs/protocol.svg)

[Home](../README.md) | [Start here](GETTING_STARTED.md) | [Server tutorial](SERVER.md) | [API reference](API.md) | [Build](BUILDING.md)

This guide builds a UDP request/reply exchange, explains datagrams and their
sender information, then covers limits, IPv6, timeouts, cancellation and the
complete public API. Copy [NCL.hpp](../NCL.hpp) into your project and use C++17.
UDP requires no third-party library; Windows links its Winsock system library.

Save each complete **File** program next to `NCL.hpp` in your application folder;
run build commands there. The `docs/` directory contains the guides, not the
working directory for compilation. [Build instructions](BUILDING.md).

## Contents

- [Understand datagrams](#understand-datagrams)
- [Prepare the project](#prepare-the-project)
- [First UDP server and client](#first-udp-server-and-client)
- [Compile and run the pair](#compile-and-run-the-pair)
- [Construct a socket and discover its port](#construct-a-socket-and-discover-its-port)
- [Build and send a packet](#build-and-send-a-packet)
- [Receive a packet and reply to its sender](#receive-a-packet-and-reply-to-its-sender)
- [Capacity, truncation and payload limits](#capacity-truncation-and-payload-limits)
- [Serve multiple senders](#serve-multiple-senders)
- [Timeouts and DNS](#timeouts-and-dns)
- [Cancel a receive from another thread](#cancel-a-receive-from-another-thread)
- [IPv4, IPv6 and dual stack](#ipv4-ipv6-and-dual-stack)
- [Use two computers on a LAN](#use-two-computers-on-a-lan)
- [Ownership and errors](#ownership-and-errors)
- [API reference](#api-reference)
- [Troubleshoot a UDP exchange](#troubleshoot-a-udp-exchange)

## Understand datagrams

UDP sends independent **datagrams**, also called packets here. A receive reads
one datagram and returns its sender's address and port. There is no listener
that accepts connections and no persistent connection to establish first.

For example, a client binds a temporary local port and sends a request packet
to the server's UDP port 6000. The server receives that packet and sends its
reply to the client's address and temporary port, obtained from reception.

Both sides use the same `DatagramSocket` type. The application calls one side
a server because it waits on a known address/port and replies to senders.

Unlike TCP, datagram boundaries are preserved. Two UDP sends create two
datagrams; a large receive buffer does not combine them. An empty datagram is
a real packet, not a disconnect or EOF signal.

> [!IMPORTANT]
> UDP does not guarantee delivery, ordering or absence of duplicates. A successful
> send means the OS accepted the packet, not that the receiver processed it.
> NCL does not add acknowledgments, retries, ordering, fragmentation or encryption.
> If your application needs those, define them in its protocol or consider
> [TCP](TCP.md) or [HTTP](HTTP.md).

## Prepare the project

Place the saved example programs next to `NCL.hpp` and include it:

```cpp
#include "NCL.hpp"
```

Use the uppercase filename on case-sensitive systems. Blocks labelled **File**
are complete programs; save each under its own filename. Other code blocks are
fragments for an already-created socket or packet. Compile programs containing
separate `main()` functions separately.

## First UDP server and client

This first exchange stays on the local machine. The server uses UDP port 6000.
The client asks the OS for a temporary local port. Both programs set timeouts
so a missing peer does not leave the example waiting indefinitely.

### Complete server

**File: `udp_server.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>

int main() {
    try {
        ncl::DatagramSocket server(6000, "127.0.0.1");
        ncl::WaitOptions wait{std::chrono::seconds(15)};
        std::cout<<"Waiting for a UDP packet on 127.0.0.1:6000"<<std::endl;
        auto packet = server.receive(2048, wait);
        std::cout<<"From "<<packet.address<<':'<<packet.port<<": ";
        std::cout.write(packet.data.data(), static_cast<std::streamsize>(packet.data.size()));
        std::cout<<'\n';
        packet.data = "Reply: " + packet.data;
        server.send(packet, wait);
    } catch (const std::exception& error) {
        std::cerr<<"Error: "<<error.what()<<'\n';
        return 1;
    }
}
```

The constructor binds immediately. `receive(2048, wait)` waits for one packet
that fits in 2048 bytes and returns a `DatagramPacket`. Changing only `data`
preserves the sender's destination information for a reply, including family
and IPv6 scope when applicable. The program sends one reply and then exits.

### Complete client

**File: `udp_client.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>

int main() {
    try {
        ncl::DatagramSocket client(0, "127.0.0.1");
        ncl::WaitOptions wait{std::chrono::seconds(5)};
        client.send({"Hello from UDP", "127.0.0.1", 6000}, wait);
        const auto reply = client.receive(2048, wait);
        if (reply.address != "127.0.0.1" || reply.port != 6000)
            throw std::runtime_error("Reply came from an unexpected endpoint");
        std::cout<<reply.data<<'\n';
    } catch (const std::exception& error) {
        std::cerr<<"Error: "<<error.what()<<'\n';
        return 1;
    }
}
```

The zero in the constructor requests a free local port. The nonzero 6000 in
the packet is the server's destination port. The same client socket must
remain open to receive the reply sent to that temporary port.

## Compile and run the pair

### Windows with GCC or MinGW

```powershell
g++ -std=c++17 -Wall -Wextra -Wpedantic udp_server.cpp -o udp_server.exe -lws2_32
g++ -std=c++17 -Wall -Wextra -Wpedantic udp_client.cpp -o udp_client.exe -lws2_32
```

From the directory containing those executables, start the server in terminal 1:

```powershell
.\udp_server.exe
```

Within its 15-second wait, start the client in terminal 2:

```powershell
.\udp_client.exe
```

The client prints `Reply: Hello from UDP`. Both programs exit after one exchange.
A timeout means the expected packet did not arrive within that operation's
budget; it does not prove which peer or network component caused the problem.

### Windows with MSVC

In a Visual Studio developer command prompt:

```bat
cl /EHsc /std:c++17 /W4 /utf-8 udp_server.cpp /Fe:udp_server.exe
cl /EHsc /std:c++17 /W4 /utf-8 udp_client.cpp /Fe:udp_client.exe
```

The header links Winsock automatically. With GCC, keep `-lws2_32` after the
source/object files. NCL handles Winsock runtime initialization itself.

### Linux or WSL

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic udp_server.cpp -o udp_server
g++ -std=c++17 -Wall -Wextra -Wpedantic udp_client.cpp -o udp_client
```

Run `./udp_server` and `./udp_client` in separate terminals. Add `-pthread`
when compiling the threaded cancellation program later in this guide.

## Construct a socket and discover its port

All of these constructors bind immediately:

```cpp
ncl::DatagramSocket automatic;
ncl::DatagramSocket local(0, "127.0.0.1");
ncl::DatagramSocket service(6000, "127.0.0.1");
const auto endpoint = local.localEndpoint();
std::cout<<endpoint.address<<':'<<endpoint.port<<'\n';
```

The default constructor binds an available IPv4 port on `0.0.0.0`. It does not
create an empty unbound object. A client does not need a known local port,
but it still owns a bound socket. `localEndpoint()` returns the actual binding;
an all-interface binding can report `0.0.0.0`, which is not a remote destination.
Ports have type `std::uint16_t`. Validate externally supplied numbers before
conversion: values outside 0..65535 can be changed by the conversion before
NCL sees them. Destination port zero is always rejected.

Two separate TCP/UDP services can use the same port number because the
transports differ. Two UDP sockets cannot assume they can share the same
binding. NCL does not expose UDP address-reuse or multicast configuration.

## Build and send a packet

`DatagramPacket` describes data and a destination when sending:

| Field | Type and meaning |
| --- | --- |
| `data` | Binary-safe `std::string` body. |
| `address` | Destination hostname or numeric address. |
| `port` | Destination `uint16_t`; must not be zero. |
| `family` | Optional `AddressFamily` filter; default Unspecified. |
| `scopeId` | IPv6 interface index, default zero. |

A default packet has empty data/address, port zero, Unspecified family and scope
zero. Set its destination before sending; a default packet alone is not a valid
destination. Received packets have their sender fields filled by NCL.

For an existing `socket`, an explicit packet is equivalent to aggregate syntax:

```cpp
ncl::DatagramPacket packet;
packet.data = "Hello";
packet.address = "127.0.0.1";
packet.port = 6000;
socket.send(packet);
```

`send` returns normally after one complete datagram is accepted, or throws.
It does not report a partial prefix as success. Resolution chooses a compatible
destination; NCL sends to the first compatible address and does not duplicate
a successful packet across other resolver results.

No null terminator or TCP message prefix is added. To include zero bytes,
construct a string with the intended length:

```cpp
ncl::DatagramPacket binary{std::string("a\0b", 3), "127.0.0.1", 6000};
socket.send(binary);
socket.send({"", "127.0.0.1", 6000});
```

These are two datagrams: three bytes and zero bytes. UDP payloads are bytes,
not C++ objects or automatically decoded text. Agree on number formats, encoding
and message fields with your peer. A string holding JSON is still just a packet
body; UDP does not automatically become HTTP by carrying it.

## Receive a packet and reply to its sender

The same fields describe the **sender** after reception:

```cpp
ncl::WaitOptions wait{std::chrono::seconds(5)};
auto packet = socket.receive(2048, wait);
std::cout<<"Sender: "<<packet.address<<':'<<packet.port<<'\n';
packet.data = "Acknowledged";
socket.send(packet, wait);
```

Preserve the received address, port, family and scope for a reply. Rebuilding
a destination from only address/port can discard the interface ID needed by
IPv6 link-local traffic. Each subsequent `receive` reads a new datagram, possibly
from a completely different sender.

`receive` blocks from the caller's perspective until a packet arrives, an error
occurs, a deadline expires or cancellation is observed. An empty received
`data` string means an empty datagram. It does not mean the sender closed its
socket; UDP has no TCP-like EOF or half-close API.

## Capacity, truncation and payload limits

Receive capacity must be from 1 to `ncl::maxDatagramSize`, which is 65507 bytes.
The default capacity is that maximum. Payloads larger than 65507 are rejected
on send with `MessageTooLarge`. The same conservative cap applies to IPv4 and
IPv6; NCL does not implement IPv6 jumbograms.

If a datagram is larger than the supplied receive capacity, NCL consumes and
discards it, then throws `DatagramTruncated`. It does not return a shortened
packet that looks complete. The next receive reads the next datagram, not the
discarded remainder. The socket stays open.

For an existing receiver:

```cpp
try {
    auto packet = socket.receive(32, wait);
    std::cout<<"Complete packet: "<<packet.data.size()<<" bytes\n";
} catch (const ncl::NetworkError& error) {
    if (error.kind() != ncl::ErrorKind::DatagramTruncated) throw;
    std::cerr<<"Discarded a packet larger than 32 bytes\n";
}
```

Increasing capacity afterward cannot recover that discarded packet. Choose a
capacity that matches your application protocol and count a truncation as a
failed application packet.

The NCL maximum is not a promise that every network will carry that size.
Large datagrams may require IP fragmentation and networks can impose smaller
limits. Start with small messages. NCL never splits one application datagram
into several packets, and does not reassemble an application-level multipart
protocol for you.

## Serve multiple senders

Keep receiving and replying on the same socket. This fragment assumes an open
server socket and intentionally treats an idle timeout as a chance to continue:

```cpp
while (true) {
    try {
        auto packet = socket.receive(2048, wait);
        packet.data = "Reply: " + packet.data;
        socket.send(packet, wait);
    } catch (const ncl::NetworkError& error) {
        if (error.kind() == ncl::ErrorKind::Timeout ||
            error.kind() == ncl::ErrorKind::DatagramTruncated) {
            continue;
        }
        throw;
    }
}
```

A UDP socket does not remember a unique client. If you need sessions, identify
them in your own packet format and track their lifetime. Do not infer that
two packets must belong to the same logical operation just because they came
from the same address/port.

When deciding to retry after a missing reply, account for duplicate delivery:
the request might have arrived while the reply was lost. An application can
include a request ID and define whether repeated requests are safe. NCL cannot
decide that policy for your protocol.

## Timeouts and DNS

`send` and `receive` accept `WaitOptions`. Use a duration in milliseconds or
convertible chrono units:

```cpp
ncl::WaitOptions wait;
wait.timeout = std::chrono::seconds(2);
```

Zero means no NCL deadline, positive means an operation budget, and negative
durations are rejected with `InvalidArgument`. Each method call starts its
own budget; a two-second send followed by a two-second receive is not a shared
two-second request/reply transaction.

Timeout/cancellation does not close a UDP socket. A later operation can use
fresh options. Successful sending still does not imply delivery. A send timeout
means local sending could not complete in its budget, not that a remote server
explicitly refused the request.

Hostname lookup in `send` is synchronous and outside the timeout. Cancellation
is checked before and after resolution. Numeric destinations avoid hostname
lookup. The socket's binding limits which address families it can use.

## Cancel a receive from another thread

Use a cancellation token rather than closing the socket from another thread:

**File: `udp_cancel.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>
#include <thread>

int main() {
    try {
        ncl::DatagramSocket socket(0, "127.0.0.1");
        ncl::CancellationSource stop;
        ncl::WaitOptions wait{std::chrono::seconds(5), stop.token()};
        std::thread controller([stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.requestCancellation();
        });
        try {
            auto packet = socket.receive(2048, wait);
            std::cout<<"Received "<<packet.data.size()<<" bytes\n";
        } catch (const ncl::NetworkError& error) {
            controller.join();
            if (error.kind() != ncl::ErrorKind::Cancelled) throw;
            std::cout<<"Cancelled; socket remains open: "<<socket.isOpen()<<'\n';
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

Compile it with `-pthread` on GCC. The main thread owns the socket; the controller
only requests shared cancellation. If a worker owns your socket, request
cancellation and join that worker before closing or moving the socket.

Source copies share the request, which cannot be reset. Tokens keep that state
after a source is destroyed. `requestCancellation()` is true only for the first
request. Empty tokens cannot be cancelled. A pre-cancelled token rejects a
call before I/O. Socket waits check tokens at up to 20 ms intervals plus
scheduling delays; synchronous DNS cannot be interrupted this way. Completion
can win a race, and cancellation does not undo a packet already sent/consumed.

## IPv4, IPv6 and dual stack

The constructor's binding selects the socket family. A normal IPv4 socket does
not switch to IPv6 for a send. To exchange IPv6 loopback traffic:

```cpp
ncl::DatagramSocket server6(6000, "::1");
ncl::DatagramSocket client6(0, "::1");
client6.send({"Hello IPv6", "::1", 6000}, wait);
auto packet6 = server6.receive(2048, wait);
packet6.data = "IPv6 reply";
server6.send(packet6, wait);
```

An IPv6 binding defaults to `IPv6Mode::IPv6Only`. To handle IPv4 and IPv6 on
one socket/port, bind `::` with explicit dual stack:

```cpp
ncl::DatagramSocket both(6000, "::", ncl::IPv6Mode::DualStack);
```

Requesting dual stack on an IPv4 binding is invalid. Binding `::1` remains
specific to IPv6 loopback. A separate IPv4 socket can bind the same port as an
IPv6-only socket; a dual-stack binding occupies that port for both families.

On dual-stack reception, an IPv4-mapped peer is normalized to an ordinary IPv4
address/family. Replying with the returned packet works: NCL maps the destination
back into the format required by the IPv6 socket. IPv6-only UDP rejects mapped
IPv4 destinations. Moving a socket preserves its family/dual-stack behavior.

The packet's optional family filters destination resolution:

```cpp
client6.send({"Hello", "localhost", 6000, ncl::AddressFamily::IPv6}, wait);
```

`Unspecified` means resolve compatibly with the bound socket, not that an IPv4
socket becomes IPv6. An incompatible explicit filter is rejected.

### IPv6 scope IDs

Link-local addresses may need an interface index. A numeric address zone such
as `fe80::1234%7` or a nonzero `packet.scopeId` can supply it; use the actual
interface index. If both are nonzero they must agree. Received packets preserve
the scope in both address and `scopeId`. IPv4 destinations do not accept IPv6
scope IDs. Interface-name zones depend on the OS; numeric zones are portable.

Keep these fields when replying. Real link-local interface exchanges are not
declared tested; synthetic formatting/resolution checks do not establish that
your network is configured for them.

## Use two computers on a LAN

Bind the server to `0.0.0.0` instead of loopback. Set the client's destination
to the server computer's actual IPv4 address. Find it with `ipconfig` on Windows
or `ip -4 addr` on Linux. For example, `192.168.1.50` is a destination only if
that is really the server's address on your network.

Allow the application's **UDP** port through the relevant private-network
firewall when needed. A TCP firewall rule for the same number is not a UDP
rule. Guest networks can isolate clients. `127.0.0.1` and `::1` never refer to
another computer, while `0.0.0.0` and `::` are binding addresses, not normal
remote destinations.

For IPv6 use a real reachable IPv6 destination and an appropriate IPv6 binding.
Dual stack alone does not make unavailable connectivity work.

## Ownership and errors

`DatagramSocket` cannot be copied. It can be moved, transferring its bound
resource and leaving the source closed. Destruction and `close()` release the
resource; explicit close is idempotent and noexcept. Replace a closed UDP socket
with a new object rather than trying to reconnect or rebind it through methods
that do not exist.

Use each object from one thread at a time. NCL does not synchronize concurrent
send, receive, move or close on the same object. Different objects can have
different owning threads.

`NetworkError` provides `what()`, `kind()`, `nativeCode()` and
`bytesTransferred()`. The progress count is for TCP calls; it is not a count
of delivered UDP bytes or remote acknowledgments. Native codes are Winsock/errno
for System, resolver codes for Resolution, or zero for NCL-only errors.

| Error category | UDP meaning / action |
| --- | --- |
| `InvalidArgument` | Bad capacity, zero destination port, invalid timeout, incompatible family/scope or invalid host input. Correct the arguments. |
| `InvalidState` | Operation attempted on a closed/moved-from socket. |
| `MessageTooLarge` | Outgoing body exceeds 65507 bytes; rejected before sending. |
| `DatagramTruncated` | Incoming packet exceeded capacity and was discarded; socket stays open. |
| `Resolution` | Hostname/address lookup failed. |
| `Timeout` | NCL operation budget expired; socket stays open. |
| `Cancelled` | Token was cancelled; socket stays open. |
| `System` | Native networking failure; inspect the native code and application context. |

UDP native behavior can include errors after sending to a port with no receiver;
it is not a portable application-level acknowledgment. Do not infer delivery
or remote liveness from either a successful send or the absence of an error.
Standard exceptions such as `std::bad_alloc` retain their type. NCL's text is
English; native text can follow the OS language.

## API reference

### DatagramSocket

The constructor takes `std::uint16_t port`, `const std::string& bindAddress`
and `IPv6Mode mode`. `send` takes `const DatagramPacket&` and returns `void`;
`receive` takes a `std::size_t` capacity and returns `DatagramPacket` by value.
Both take `WaitOptions` by value. No reference to a sent packet is retained
after the synchronous call returns.

| Constructor or method | Result |
| --- | --- |
| `DatagramSocket(port = 0, bindAddress = "0.0.0.0", mode = IPv6Mode::IPv6Only)` | Bind immediately; local port zero chooses a free port. |
| `send(packet, options = {})` | Send one datagram or throw; no delivery acknowledgment. |
| `receive(capacity = maxDatagramSize, options = {})` | Return one complete `DatagramPacket`, or throw on truncation/failure. |
| `localEndpoint()` | Return an `Endpoint` with bound address/family and actual local port; requires an open socket. |
| `isOpen()` | Return `bool` for local handle ownership; noexcept. |
| `close()` | Release the handle; idempotent, noexcept. |

There is no UDP `accept`, `connect`, `remoteEndpoint`, `receiveMessage`,
`shutdownInput`, `shutdownOutput` or EOF API in this version.

### Values and constants

| Name | Definition |
| --- | --- |
| `DatagramPacket` | `data`, `address`, `port`, `family`, `scopeId`, in that order. |
| `Endpoint` | `address`, `port`, `family`, `scopeId`. |
| `maxDatagramSize` | 65507 bytes. |
| `AddressFamily` | Unspecified, IPv4, IPv6. |
| `IPv6Mode` | IPv6Only, DualStack. |
| `WaitOptions` | `timeout` milliseconds default 0 and `cancellation` default empty token. |
| `CancellationSource` | `token()` and one-way `requestCancellation()`; copies share state. |
| `CancellationToken` | `canBeCancelled()` and `isCancellationRequested()`. |

`DatagramPacket` is a copyable aggregate in the field order shown above:
`std::string data`, `std::string address`, `std::uint16_t port`,
`AddressFamily family`, `std::uint32_t scopeId`. `maxDatagramSize` is an
`inline constexpr std::size_t`. Exact shared-type signatures and the complete
`ErrorKind` enum are in the [TCP reference](TCP.md#shared-value-types) and
[network error table](TCP.md#errors-progress-and-retry-decisions).

## Troubleshoot a UDP exchange

| Symptom | Check |
| --- | --- |
| Client receives nothing | Start the server first; check the destination and UDP port; keep the client socket open for its reply. |
| Reply goes to the wrong port | Reply to the received sender port, not the server's own port. |
| `DatagramTruncated` | Increase capacity to your protocol maximum; the rejected packet has already been discarded. |
| IPv6 destination fails on the default socket | Create an IPv6 or dual-stack binding first. |
| Empty payload mistaken for disconnect | Empty datagrams are valid; UDP has no EOF. |
| Packets missing, duplicated or out of order | That must be handled by the application protocol; it is not repaired by repeated receives. |
| Local exchange succeeds but LAN fails | Verify real LAN addresses, UDP firewall rules and device isolation. |
| Received bytes contain garbage when printed | Use `data.size()` or string output; do not assume a received C buffer has a terminator. |

The [README verification scope](../README.md#status-and-limits)
describes tested systems. Local loopback tests do not verify physical LAN
behavior or induced packet loss/reordering. Continue with [TCP](TCP.md) for a
byte stream or [HTTP](HTTP.md) for requests, endpoint servers and Swagger.
