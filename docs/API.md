# NCL public API reference

![Api Reference](../assets/docs/reference.svg)

[Home](../README.md) | [Start here](GETTING_STARTED.md) | [Server tutorial](SERVER.md) | **API reference** | [Build](BUILDING.md)

This reference covers the public application API in `NCL.hpp` in the
1.0.0-beta.1 release. All names below belong to `ncl` unless marked `std`.

> [!NOTE]
> Types/functions inside **`ncl::detail` are implementation details**, including
> the crypto/TLS engine; applications should not call them.

Signatures omit the `ncl::` prefix. `options = {}` means a default-constructed
options value, not saved configuration from an earlier operation.

## Contents

- [Constants and build switch](#constants-and-build-switch)
- [Address and packet values](#address-and-packet-values)
- [Timeouts and cancellation](#timeouts-and-cancellation)
- [Socket](#socket)
- [ServerSocket](#serversocket)
- [DatagramSocket](#datagramsocket)
- [HttpHeaders](#httpheaders)
- [HTTP request and response values](#http-request-and-response-values)
- [HttpOptions](#httpoptions)
- [HttpRouteOptions](#httprouteoptions)
- [HttpClient](#httpclient)
- [HttpServer](#httpserver)
- [Errors](#errors)
- [Ownership and lifetimes](#ownership-and-lifetimes)

## Constants and build switch

| Name | Type / value | Meaning |
| --- | --- | --- |
| `defaultMaxMessageSize` | `inline constexpr std::size_t`, `1024 * 1024` | Default **payload** cap for TCP message helpers. |
| `maxDatagramSize` | `inline constexpr std::size_t`, `65507` | Shared NCL IPv4/IPv6 UDP payload cap; not a recommended Internet packet size. |
| `NCL_ENABLE_TLS` | Preprocessor definition, absent by default | Enables built-in TLS/HTTPS when defined before the first include in every translation unit. Defining it as `0` also enables it. |

There is no runtime TLS backend selector, `NCL_IMPLEMENTATION`, public TLS
socket or public primitive-cryptography API. See [TLS profile](TLS.md).

## Address and packet values

| Type | Members / default |
| --- | --- |
| `AddressFamily` | Enum: `Unspecified`, `IPv4`, `IPv6`. |
| `IPv6Mode` | Enum: `IPv6Only`, `DualStack`. Dual stack requires an IPv6 listener binding. |
| `Endpoint` | `std::string address` empty; `std::uint16_t port = 0`; `AddressFamily family = Unspecified`; `std::uint32_t scopeId = 0`. |
| `DatagramPacket` | `std::string data` empty; `std::string address` empty; `std::uint16_t port = 0`; `AddressFamily family = Unspecified`; `std::uint32_t scopeId = 0`. |

These are copyable data values. Returned endpoints contain numeric addresses.
Ports are host-order values. `scopeId` carries an IPv6 interface index; preserve
it when replying to a scoped UDP sender. `data` and message/body strings are
binary-safe, including embedded NUL bytes; no encoding conversion occurs.

Listener port 0 requests an OS-selected port; call `localEndpoint().port`
to discover it. Destination port 0 is invalid. Validate external integers
before converting to `std::uint16_t`.

## Timeouts and cancellation

### WaitOptions

| Field | Type / default | Meaning |
| --- | --- | --- |
| `timeout` | `std::chrono::milliseconds{0}` | Zero means no deadline; negative durations are invalid. |
| `cancellation` | `CancellationToken{}` | Default token cannot be cancelled. |

Options are passed **by value** to individual calls. A positive timeout is a
whole-call budget, shared across partial transfers and connection attempts.
It is not an idle timer reset after every byte. Synchronous DNS resolution
is outside the socket budget. A new API call gets a new budget.

### CancellationSource and CancellationToken

| Signature | Result |
| --- | --- |
| `CancellationSource()` | Create a shared, initially unrequested cancellation state; allocation can throw. |
| `CancellationToken token() const noexcept` | Obtain a token sharing that state. |
| `bool requestCancellation() const noexcept` | Request stopping. Returns true only for the first request on that state. |
| `CancellationToken() noexcept` | Construct a token with no source. |
| `bool canBeCancelled() const noexcept` | Whether a token has a source. |
| `bool isCancellationRequested() const noexcept` | Whether its source was requested. |

Source/token copies share state. There is no reset: create a new source for
a new operation group. The supported cross-thread operation is requesting
cancellation through a source whose lifetime is stable; socket ownership
stays with its I/O thread. Cancellation is cooperative and does not undo
completed sends or application state changes. See [safe stopping](SERVER.md#step-8-stop-the-server-cleanly).

## Socket

A connected TCP stream. Move-only with automatic resource cleanup. It stores
no default per-operation timeout. See [TCP walkthrough](TCP.md).

| Signature | Behavior |
| --- | --- |
| `Socket() noexcept` | Empty object, ready for a later `connect`. |
| `Socket(const std::string& host, std::uint16_t port, WaitOptions options = {}, AddressFamily family = AddressFamily::Unspecified)` | Resolve and connect. |
| `void connect(const std::string& host, std::uint16_t port, WaitOptions options = {}, AddressFamily family = AddressFamily::Unspecified)` | Requires an empty/closed socket; tries compatible resolver results in order. |
| `bool isOpen() const noexcept` | Whether the local object owns a socket. |
| `bool isConnected() const noexcept` | Same local state as `isOpen`; does not probe peer liveness. |
| `bool hasReachedEof() const noexcept` | Whether a receive observed remote orderly EOF. Local input shutdown alone does not set it. |
| `bool isInputShutdown() const noexcept` | Whether input was explicitly shut down locally. |
| `bool isOutputShutdown() const noexcept` | Whether output was explicitly shut down locally. |
| `void shutdownInput()` | Local input half-close; later reads report EOF, tracked separately from observed remote EOF. |
| `void shutdownOutput()` | Local output half-close; prevents further sending while input remains usable. |
| `void close() noexcept` | Idempotently release the handle and reset EOF/shutdown flags. |
| `Endpoint localEndpoint() const` | Local binding; requires an open socket. |
| `Endpoint remoteEndpoint() const` | Connected peer; requires an open socket. |
| `std::size_t sendSome(const void* data, std::size_t size, WaitOptions options = {})` | Send up to `size` bytes and return the count. Size 0 is a no-op on an open output. |
| `void sendAll(const void* data, std::size_t size, WaitOptions options = {})` | Send exactly `size` bytes or throw. |
| `std::size_t receive(void* buffer, std::size_t capacity, WaitOptions options = {})` | Receive 1..capacity bytes; 0 means orderly EOF. Capacity must be positive. |
| `bool receiveExact(void* buffer, std::size_t size, WaitOptions options = {})` | Fill exactly `size`; false for EOF before any byte, exception for partial EOF. Size 0 succeeds. |
| `void sendMessage(std::string_view message, std::size_t maxBytes = defaultMaxMessageSize, WaitOptions options = {})` | Send a four-byte big-endian payload length and then the payload. |
| `std::optional<std::string> receiveMessage(std::size_t maxBytes = defaultMaxMessageSize, WaitOptions options = {})` | Receive one frame. `nullopt` means clean EOF before its prefix; an empty string is a valid message. |

Nonzero buffer sizes require non-null pointers to sufficient storage.
`sendAll`/`receiveExact` do not add framing. TCP may split/coalesce native
transfers; it has no application message boundaries of its own.

Message caps apply to payload bytes; the four-byte prefix is additional. The
wire limit is `UINT32_MAX`. A partially transferred frame cannot be retried
as a fresh message on the same connection. Network failures can close the
socket; use `bytesTransferred()` and the [TCP failure rules](TCP.md#errors-progress-and-retry-decisions).
Destruction and noexcept move construction/assignment transfer/release ownership;
copy construction/assignment are deleted.

## ServerSocket

A TCP listener. Move-only; the destructor releases it.

| Signature | Behavior |
| --- | --- |
| `explicit ServerSocket(std::uint16_t port, const std::string& bindAddress = "0.0.0.0", int backlog = 16, IPv6Mode mode = IPv6Mode::IPv6Only)` | Bind and listen immediately. Positive backlog required. |
| `Socket accept(WaitOptions options = {})` | Wait for and return an independently owned connected socket. |
| `bool isOpen() const noexcept` | Whether the listener is owned. |
| `void close() noexcept` | Idempotently release only the listener. |
| `Endpoint localEndpoint() const` | Actual binding, including an automatically assigned port. |

`ServerSocket` defaults to **all IPv4 interfaces**, while `HttpServer` defaults
to loopback. Closing a listener does not close already accepted sockets.
There is no reopen/rebind method; construct a replacement listener. Move
construction/assignment are noexcept; copying is deleted.

## DatagramSocket

A bound UDP socket. Move-only with noexcept moves and destructor cleanup.
See [UDP tutorial](UDP.md).

| Signature | Behavior |
| --- | --- |
| `explicit DatagramSocket(std::uint16_t port = 0, const std::string& bindAddress = "0.0.0.0", IPv6Mode mode = IPv6Mode::IPv6Only)` | Bind immediately, by default to an ephemeral IPv4 port on all interfaces. |
| `void send(const DatagramPacket& packet, WaitOptions options = {})` | Send one complete packet to its destination; no delivery acknowledgment. |
| `DatagramPacket receive(std::size_t capacity = maxDatagramSize, WaitOptions options = {})` | Consume one packet and return its payload plus sender endpoint fields. |
| `bool isOpen() const noexcept` | Whether the socket is owned. |
| `void close() noexcept` | Idempotently release it. |
| `Endpoint localEndpoint() const` | Actual local binding. |

Receive capacity must be 1..65507. An oversized incoming packet is consumed
and rejected with `DatagramTruncated`; partial payload is not returned.
An empty datagram is valid and is not EOF. The cap does not guarantee a
packet fits the path MTU; UDP loss, duplication, reordering and retries are
application concerns. There is no connected-UDP/multicast/broadcast API here.

## HttpHeaders

An ordered, copyable collection preserving duplicates. Header-name lookup is
ASCII case-insensitive. Invalid header names/values throw `HttpError`.

| Signature | Behavior |
| --- | --- |
| `using Field = std::pair<std::string, std::string>` | Name/value pair type. |
| `HttpHeaders()` | Empty collection. |
| `HttpHeaders(std::initializer_list<Field> fields)` | Add each validated field in order. |
| `void add(std::string name, std::string value)` | Append without removing duplicates. |
| `void set(std::string name, std::string value)` | Erase all matching names, then append one field. |
| `void erase(std::string_view name)` | Remove all matching fields. |
| `std::optional<std::string> get(std::string_view name) const` | Copy the first value; `nullopt` if missing. |
| `std::vector<std::string> getAll(std::string_view name) const` | Copy every matching value in order. |
| `bool contains(std::string_view name) const` | Whether at least one matching field exists. |
| `std::size_t size() const noexcept` | Number of fields, including duplicates. |
| `begin() const noexcept`, `end() const noexcept` | Const iterators for range-based `for`. |

Lookups return independent copies. Mutating the collection can invalidate
iterators. Header collection validity does not override HTTP message rules:
NCL owns managed framing fields and restricts some headers when serializing.
See [HTTP header rules](HTTP.md#header-rules-and-message-bodies).

## HTTP request and response values

### HttpRequest

| Field | Type / default |
| --- | --- |
| `method` | `std::string`, `"GET"`. |
| `headers` | Empty `HttpHeaders`. |
| `body` | Empty `std::string`, binary-safe. |

Use `HttpClient::request` for PUT/PATCH/DELETE/OPTIONS/custom method tokens.
Server route registration supports the seven methods listed below; a valid
client method is not automatically a supported server route method.

### HttpResponse

| Field | Type / default |
| --- | --- |
| `version` | Empty `std::string`; populated by the client parser. Server responses use the request's supported version. |
| `statusCode` | `int`, `0`. Set it or use a factory when returning a server response. |
| `reasonPhrase` | Empty `std::string`; server supplies a known phrase when possible. |
| `headers` | Empty `HttpHeaders`. |
| `body` | Empty binary-safe `std::string`. |
| `trailers` | Empty `HttpHeaders`; populated for incoming chunked responses, unsupported on outgoing server responses. |

| Static factory | Result |
| --- | --- |
| `HttpResponse text(std::string content, int status = 200)` | Body with `text/plain; charset=utf-8`. |
| `HttpResponse json(std::string content, int status = 200)` | Body labeled `application/json`. Content must already be valid JSON. |
| `HttpResponse empty(int status = 204)` | Bodyless response with the supplied status. |

Factories construct values; serialization enforces the final protocol rules.
Server statuses must be 200..599; 204/205/304 cannot carry body content.
HEAD suppresses the body on the wire but still executes the selected handler.
JSON construction does not escape values, validate syntax or serialize objects.

### HttpServerRequest

Inherits `method`, `headers` and `body` from `HttpRequest`, then adds:

| Field | Type / meaning |
| --- | --- |
| `version` | `std::string`; incoming HTTP/1.0 or HTTP/1.1. |
| `target` | `std::string`; original request target. |
| `path` | `std::string`; decoded path without query. |
| `query` | `std::string`; original query text without `?`. |
| `trailers` | `HttpHeaders`; separately parsed incoming trailers. |
| `remote` | `Endpoint`; numeric peer address/port/family/scope. |
| `pathParameters` | `std::vector<std::pair<std::string, std::string>>`; captured template values. |
| `queryParameters` | Same pair-vector type; decoded query pairs, retaining duplicates. |
| `pathSegments` | `std::vector<std::string>`; decoded segments used by routing. |

Extra fields are empty/default before parsing. Accessors are const:

| Signature | Result |
| --- | --- |
| `std::optional<std::string> pathParam(std::string_view name) const` | First captured value, or `nullopt`. |
| `std::optional<std::string> queryParam(std::string_view name) const` | First query value, or `nullopt`. |
| `std::vector<std::string> queryParams(std::string_view name) const` | All query values, or empty vector. |

Parameter-name lookup is case-sensitive. Query `+` decodes to a space; path
`+` stays literal. A present empty value differs from a missing value. Decoding
occurs once; see [parsing and routing rules](HTTP.md#read-path-query-headers-and-body).

## HttpOptions

Passed by value to client calls and copied into a server at construction.

| Field | Type / default | Scope |
| --- | --- | --- |
| `wait` | `WaitOptions{}` | Client whole operation; server accepted request. |
| `maxHeaderBytes` | `std::size_t`, `32768` | Start line plus header bytes/CRLF per section; also bounds trailers. |
| `maxBodyBytes` | `std::size_t`, `8388608` | Incoming and outgoing complete body cap. |
| `maxChunkMetadataBytes` | `std::size_t`, `262144` | Incoming chunk framing/extensions/trailers budget. |
| `maxInformationalResponses` | `std::size_t`, `8` | Client cap before the final response. |
| `caFile` | `std::string`, empty | Client HTTPS trust: empty selects system roots; non-empty PEM file replaces them. |
| `family` | `AddressFamily::Unspecified` | Client destination resolver-family filter. |

Zero size/count limits are literal caps, not unlimited values. Limits count
bytes, not Unicode characters. Request/response bodies are fully buffered;
a larger cap does not enable streaming. `caFile` and `family` do not configure
the server's certificate or binding. See [budgets](HTTP.md#configure-limits-and-operation-budgets).

## HttpRouteOptions

Copied at route registration. Metadata affects OpenAPI/Swagger only; it does
not validate bodies, require query fields, or set response status/content type.

| Field | Type / default |
| --- | --- |
| `summary` | Empty `std::string`. |
| `requestContentType` | Empty `std::string`; non-empty describes a request body. |
| `responseContentType` | `std::string`, `"application/json"`. |
| `successStatus` | `int`, `200`; documented range 200..599. |
| `requestBodyRequired` | `bool`, `false`. |
| `queryParameters` | Empty `std::vector<std::string>`; described as optional strings. |

Metadata must be valid UTF-8; empty/repeated query names are rejected. Path
parameters are inferred from templates and documented as required strings.
See [OpenAPI metadata behavior](HTTP.md#describe-endpoints-for-swagger).

## HttpClient

Default-constructible, synchronous and stateless between requests. Each call
opens a fresh connection. There is no connection pool, redirect/retry policy,
cookie store or decompression. All request methods below are `const` and return
`HttpResponse` by value.

| Signature | Behavior |
| --- | --- |
| `static constexpr bool supportsHttps() noexcept` | Whether this build has `NCL_ENABLE_TLS`. |
| `HttpResponse get(const std::string& url, HttpOptions options = {}) const` | GET request. |
| `HttpResponse head(const std::string& url, HttpOptions options = {}) const` | HEAD request; response body is empty. |
| `HttpResponse post(const std::string& url, std::string body, std::string contentType = "application/octet-stream", HttpOptions options = {}) const` | POST request with one content-type field. |
| `HttpResponse request(const std::string& url, const HttpRequest& request, HttpOptions options = {}) const` | Custom request. |

URLs must be complete ASCII HTTP(S) URLs. Default ports are 80/443; IPv6 URLs
use brackets. URL credentials, proxy settings, raw Unicode hostnames/IDNA and
IPv6 URL scope identifiers are unsupported. Host/connection/framing headers
are generated by NCL. No insecure certificate bypass is exposed.

4xx/5xx statuses are responses, not `HttpError` exceptions. A request can have
changed remote state even if reading its response fails. NCL never retries
automatically. See [request tutorial](HTTP.md#create-and-inspect-client-requests).

## HttpServer

A move-only listener/router. The constructor binds immediately. It processes
one connection at a time and one request per connection, with no worker pool,
persistent connections or streaming bodies.

`using Handler = std::function<HttpResponse(const HttpServerRequest&)>`.

| Signature | Behavior |
| --- | --- |
| `explicit HttpServer(std::uint16_t port = 8080, const std::string& bindAddress = "127.0.0.1", HttpOptions options = {}, int backlog = 16, IPv6Mode mode = IPv6Mode::IPv6Only)` | Create a listener and store request options. |
| `void route(std::string method, std::string path, Handler handler, HttpRouteOptions options = {})` | Register GET, HEAD, POST, PUT, PATCH, DELETE or OPTIONS, uppercase. |
| `void get(std::string path, Handler handler, HttpRouteOptions options = {})` | Register GET. |
| `void post(std::string path, Handler handler, HttpRouteOptions options = {})` | Register POST. |
| `void put(std::string path, Handler handler, HttpRouteOptions options = {})` | Register PUT. |
| `void patch(std::string path, Handler handler, HttpRouteOptions options = {})` | Register PATCH. |
| `void remove(std::string path, Handler handler, HttpRouteOptions options = {})` | Register DELETE; `delete` is a C++ keyword. |
| `void enableTls(std::string certificateFile, std::string privateKeyFile)` | Validate a PEM chain and matching unencrypted RSA key; use TLS for accepted connections. |
| `void enableSwagger(std::string title = "NCL API", std::string version = "1.0.0", std::string assetsBase = "https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.34.1")` | Enable `/swagger`, `/swagger/`, `/swagger/index.html` and `/openapi.json`. |
| `std::string openApi() const` | Return generated OpenAPI 3.0.3 JSON, even without enabling Swagger routes. |
| `void serve(CancellationToken cancellation = {})` | Repeat serving until closed or the supplied token is requested. |
| `void serveOnce(WaitOptions acceptWait = {})` | Accept/process one connection; normal return does not imply a successful handler. |
| `Endpoint localEndpoint() const` | Actual binding; requires an open listener. |
| `bool isOpen() const noexcept` | Whether the listener is owned. |
| `void close() noexcept` | Idempotently release the listener on its owning thread. |

Configure routes/TLS/Swagger before serving. Reconfiguration or recursive
serving from a handler is rejected. Configure again only after the serving
call returns. There is no route removal, wildcard/catch-all route, automatic
static-folder server, TLS disable or Swagger disable method.

Concrete paths take precedence over parameter paths. Parameters occupy whole
segments, such as `/users/{id}`. Duplicate/ambiguous routes are rejected;
path/method matching is case-sensitive and trailing slashes matter.
Unknown paths produce 404; wrong methods produce 405 with `Allow`. HEAD falls
back to GET, including its handler side effects. OPTIONS is generated unless
explicitly registered.

`serveOnce`'s accept timeout is separate from the accepted request budget
in `HttpOptions.wait`. A non-default accept cancellation token also replaces
the accepted request token. `serve(token)` passes its token to both stages;
only cancellation of that supplied token is converted to a normal return.
Setting only `options.wait.cancellation` cannot interrupt an idle accept.
An accepted client timeout/failure normally releases that connection;
accept failures still propagate. See [server error behavior](HTTP.md#server-rejection-behavior).

`close()` inside a handler stops subsequent accepts; the current response
can still finish. A remote stop endpoint would require application access
controls; use [local cancellation](SERVER.md#step-8-stop-the-server-cleanly)
for the tutorial. Destruction releases the listener. Moves are noexcept;
copy construction/assignment are deleted.

## Errors

Both exceptions derive from `std::runtime_error`; `what()` supplies a message.
Catch by `const` reference and use enum kinds for program logic.

### NetworkError and ErrorKind

`NetworkError(ErrorKind kind, const std::string& message, int nativeCode = 0,
std::size_t bytesTransferred = 0)`.

Accessors: `ErrorKind kind() const noexcept`, `int nativeCode() const noexcept`,
`std::size_t bytesTransferred() const noexcept`.

| `ErrorKind` value | Meaning |
| --- | --- |
| `System` | Socket/system failure; nativeCode is Winsock/errno. |
| `Resolution` | Address lookup failed; nativeCode is a resolver code. |
| `InvalidArgument` | Invalid port, size, buffer, wait option, address or other argument. |
| `InvalidState` | Operation incompatible with current ownership/shutdown state. |
| `MessageTooLarge` | TCP frame or outgoing UDP payload exceeds its cap. |
| `UnexpectedEof` | EOF in the middle of a required byte sequence/frame. |
| `DatagramTruncated` | Incoming UDP packet exceeded capacity and was discarded. |
| `Timeout` | Operation deadline expired. |
| `Cancelled` | Cancellation was requested. |

Other kinds normally have native code 0. Transferred-byte counts belong to
the current call and include TCP framing bytes for message helpers; they
are not a peer-delivery acknowledgment. A timeout does not prove that a remote
operation did not happen. Follow [connection-state/retry rules](TCP.md#errors-progress-and-retry-decisions).

### HttpError and HttpErrorKind

`HttpError(HttpErrorKind kind, const std::string& message)` with accessor
`HttpErrorKind kind() const noexcept`.

| `HttpErrorKind` value | Meaning |
| --- | --- |
| `InvalidArgument` | Invalid HTTP API configuration, header, route or metadata. |
| `InvalidUrl` | Invalid or unsupported URL form. |
| `InvalidResponse` | Malformed/ambiguous response or invalid outgoing server response. |
| `HeadersTooLarge` | Header/trailer/chunk metadata budget exceeded. |
| `BodyTooLarge` | Incoming/outgoing body cap exceeded. |
| `Unsupported` | Unsupported feature/encoding; includes HTTPS when TLS is disabled. |
| `Tls` | TLS handshake, record, key-file or protocol failure. |
| `Certificate` | Unsupported, malformed, untrusted, expired or wrong-identity certificate. |
| `InvalidRequest` | Malformed incoming HTTP request. |
| `UnsupportedVersion` | Unsupported incoming HTTP version. |
| `ExpectationFailed` | Unsupported request Expect behavior. |

HTTP operations can also throw `NetworkError` for connection/DNS/deadline/
cancellation failures. File and certificate failures have diagnostics that
may describe a specific constraint; use the broad enum as a category rather
than assuming every TLS problem has one universal error kind. Allocation and
other standard exceptions can also propagate.

## Ownership and lifetimes

- `Socket`, `ServerSocket`, `DatagramSocket` and `HttpServer` have one owner at
  a time. Move when quiescent; do not concurrently read/write/close/move an object.
- `HttpServer` retains handlers and copies route metadata/options. Captured
  references must outlive serving and any move that preserves those captures.
- A handler's request reference is valid only during that invocation. Copy
  data you need to retain; do not keep pointers/references into the request.
- Request/response/header/endpoint/packet/options values are ordinary copyable
  data. Modify them under normal C++ lifetime and threading rules.
- RAII releases local handles on normal scope exit and exception unwinding.
  A forcefully terminated process does not run C++ destructors.

For explanations and complete programs, return to [Getting started](GETTING_STARTED.md),
[Server](SERVER.md), [TCP](TCP.md), [UDP](UDP.md) or [HTTP](HTTP.md).
