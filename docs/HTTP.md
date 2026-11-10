# NCL HTTP, HTTPS and endpoint API guide

![Protocol Guide](../assets/docs/protocol.svg)

[Home](../README.md) | [Start here](GETTING_STARTED.md) | [Server tutorial](SERVER.md) | [API reference](API.md) | [Build](BUILDING.md)

> [!TIP]
> For a shorter first lesson, use [Getting started](GETTING_STARTED.md).
> For one server built in stages from HTTP to HTTPS, use the
> [server tutorial](SERVER.md). This guide provides the detailed behavior,
> larger worked examples and protocol-specific reference.

This guide covers both sides of a web API: making requests with `HttpClient`
and creating endpoints with `HttpServer`. It also explains headers, bodies,
JSON, path/query parameters, Swagger, TLS certificates, cancellation, limits
and every public HTTP type. The code is in [NCL.hpp](../NCL.hpp), requiring C++17.

Save each complete **File** program next to `NCL.hpp` in your application folder;
run build commands there. The `docs/` directory contains the guides, not the
working directory for compilation. [Build instructions](BUILDING.md).

## Contents

- [Understand the three roles](#understand-the-three-roles)
- [Read a request and response](#read-a-request-and-response)
- [Prepare and build HTTP programs](#prepare-and-build-http-programs)
- [First standalone HTTP client](#first-standalone-http-client)
- [Complete endpoint server](#complete-endpoint-server)
- [Complete client for that API](#complete-client-for-that-api)
- [Run the API and try Swagger](#run-the-api-and-try-swagger)
- [Create and inspect client requests](#create-and-inspect-client-requests)
- [Header rules and message bodies](#header-rules-and-message-bodies)
- [Define server routes and response functions](#define-server-routes-and-response-functions)
- [Read path, query, headers and body](#read-path-query-headers-and-body)
- [Build a small create-read-update-delete API](#build-a-small-create-read-update-delete-api)
- [Describe endpoints for Swagger](#describe-endpoints-for-swagger)
- [URLs and addresses](#urls-and-addresses)
- [Configure limits and operation budgets](#configure-limits-and-operation-budgets)
- [Cancel a running server safely](#cancel-a-running-server-safely)
- [Understand errors and retry policy](#understand-errors-and-retry-policy)
- [Enable HTTPS in the build](#enable-https-in-the-build)
- [HTTPS clients and trust configuration](#https-clients-and-trust-configuration)
- [HTTPS server and a local certificate](#https-server-and-a-local-certificate)
- [Server concurrency, ownership and scope](#server-concurrency-ownership-and-scope)
- [Public API reference](#public-api-reference)
- [Troubleshoot an API](#troubleshoot-an-api)
- [Verification and protocol references](#verification-and-protocol-references)

## Understand the three roles

| Component | Responsibility |
| --- | --- |
| HTTP server | Listen, match an endpoint, run application code and send its response. |
| HTTP client | Send a method/URL/headers/body, then read the response. |
| Swagger UI | A browser client that displays API documentation and sends test requests. |

The C++ client or Swagger sends a request to `HttpServer`. The server chooses
the matching handler, runs it and returns its status, headers and body to the
client that made that request. Swagger and the C++ client are two ways to call
the same handlers; the server can run without Swagger enabled.

An **endpoint** is identified by a method and path. `GET /products` and
`POST /products` are different operations even though their paths match.
`GET /products/1` can match the parameterized route `/products/{id}`.

NCL builds/speaks HTTP/1.1 and reads HTTP/1.0 or HTTP/1.1 messages. HTTP is a
standard message format over TCP. Use NCL's HTTP types for it, not
`Socket::sendMessage`, which adds a different, NCL-specific binary prefix.

## Read a request and response

A request contains a method, target, header fields and sometimes a body:

```http
POST /echo HTTP/1.1
Host: 127.0.0.1:8080
Content-Type: application/json
Content-Length: 18

{"message":"Ciao"}
```

The illustrated JSON body is 18 bytes. Real HTTP/1.1 lines end in CRLF
(`\r\n`); the empty line separates headers from body. NCL creates this framing
for you. The target contains the path and optional query, not a URL fragment.

A response contains a status code, headers and any permitted content:

```http
HTTP/1.1 200 OK
Content-Type: application/json
Content-Length: 18

{"message":"Ciao"}
```

| Term | Meaning |
| --- | --- |
| Method | The requested operation, such as GET, POST, PUT, PATCH or DELETE. |
| URL | Scheme, host, optional port, path and optional query. |
| Headers | Named metadata, for example Content-Type or Authorization. |
| Body | Content bytes: text, JSON, a file or arbitrary binary data. |
| Status | Result such as 200 success, 201 created, 204 no content, 404 not found. |

Status 404/500 is still an HTTP response. `HttpClient` returns it as
`HttpResponse`; it is not automatically a `NetworkError`. A connection failure
or malformed message is a different kind of failure.

## Prepare and build HTTP programs

Save complete blocks labelled **File** under their stated filenames, next to
`NCL.hpp`. Other C++ blocks are fragments to insert into an existing program.
Include the header exactly as `#include "NCL.hpp"`. Programs with separate
`main()` functions must be compiled into separate executables.

TCP, UDP and unencrypted HTTP have no third-party build dependencies. Windows
links Winsock. NCL initializes/cleans it up itself and tells MSVC to link it.
With GCC/MinGW, put `-lws2_32` after the source/object files.

### Windows with GCC or MinGW

For a saved `http_example.cpp`:

```powershell
g++ -std=c++17 -Wall -Wextra -Wpedantic http_example.cpp -o http_example.exe -lws2_32
```

Replace source/output names to build another complete example below.

### Windows with MSVC

In a Visual Studio developer command prompt:

```bat
cl /EHsc /std:c++17 /W4 /utf-8 http_example.cpp /Fe:http_example.exe
```

### Linux or WSL

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic http_example.cpp -o http_example
```

Run with `./http_example`. Add `-pthread` for application programs that create
`std::thread`. Use the exact uppercase header filename. HTTPS additionally
requires enabling the optional [built-in TLS feature](#enable-https-in-the-build).

## First standalone HTTP client

This program accepts one URL, prints the status and body, and uses a ten-second
operation budget. Save it and build it using the commands above.

**File: `http_example.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr<<"Usage: http_example <http://host/path or https://host/path>\n";
        return 1;
    }
    try {
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(10);
        const auto response = ncl::HttpClient{}.get(argv[1], options);
        std::cout<<response.statusCode<<' '<<response.reasonPhrase<<'\n';
        std::cout.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        std::cout<<'\n';
    } catch (const std::exception& error) {
        std::cerr<<"Error: "<<error.what()<<'\n';
        return 1;
    }
}
```

To test without an external service, start a local Python server in another
terminal, from a directory whose files you want it to expose:

```sh
python -m http.server 8000 --bind 127.0.0.1
```

Then call it from Windows:

```powershell
.\http_example.exe http://127.0.0.1:8000/
```

Or Linux:

```sh
./http_example http://127.0.0.1:8000/
```

Python is only an optional test peer; NCL applications do not depend on it.
The server must stay running while the client calls it. `127.0.0.1` refers to
the current computer, so this command does not contact another machine.

## Complete endpoint server

The next program resembles a small endpoint API: application functions are
registered for particular methods and paths. It listens locally on port 8080
by default and sets a five-second budget for each accepted request.

**File: `rest_server_example.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 1 && argc != 2 && argc != 4) {
        std::cerr<<"Usage: rest_server_example [port] [certificate.pem private-key.pem]\n";
        return 1;
    }
    try {
        int port = 8080;
        if (argc >= 2) {
            std::size_t end = 0;
            port = std::stoi(argv[1], &end);
            if (end != std::string(argv[1]).size() || port < 0 || port > 65535)
                throw std::runtime_error("Port must be between 0 and 65535");
        }
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(5);
        ncl::HttpServer server(static_cast<std::uint16_t>(port), "127.0.0.1", options);
        if (argc == 4) server.enableTls(argv[2], argv[3]);

        ncl::HttpRouteOptions docs;
        docs.summary = "Check that the API is running";
        server.get("/health", [](const ncl::HttpServerRequest&) {
            return ncl::HttpResponse::json(R"({"status":"ok"})");
        }, docs);

        docs.summary = "List the example products";
        server.get("/products", [](const ncl::HttpServerRequest&) {
            return ncl::HttpResponse::json(R"([{"id":1,"name":"Keyboard"},{"id":2,"name":"Mouse"}])");
        }, docs);

        docs.summary = "Read a product by its ID";
        server.get("/products/{id}", [](const ncl::HttpServerRequest& request) {
            if (request.pathParam("id") == std::optional<std::string>{"1"})
                return ncl::HttpResponse::json(R"({"id":1,"name":"Keyboard"})");
            if (request.pathParam("id") == std::optional<std::string>{"2"})
                return ncl::HttpResponse::json(R"({"id":2,"name":"Mouse"})");
            return ncl::HttpResponse::json(R"({"error":"Product not found"})", 404);
        }, docs);

        docs.summary = "Send JSON and receive the same bytes back";
        docs.requestContentType = "application/json";
        docs.requestBodyRequired = true;
        server.post("/echo", [](const ncl::HttpServerRequest& request) {
            // Echo does not parse or validate JSON. Application endpoints can use their chosen JSON library.
            auto response = ncl::HttpResponse::text(request.body);
            response.headers.set("Content-Type", request.headers.get("Content-Type").value_or("application/octet-stream"));
            return response;
        }, docs);
        server.enableSwagger("NCL example API", "1.0.0");
        port = server.localEndpoint().port;
        const std::string scheme = argc == 4 ? "https" : "http";
        std::cout<<"API: "<<scheme<<"://127.0.0.1:"<<port<<'\n';
        std::cout<<"Swagger: "<<scheme<<"://127.0.0.1:"<<port<<"/swagger"<<std::endl;
        server.serve();
    } catch (const std::exception& error) {
        std::cerr<<"Error: "<<error.what()<<'\n';
        return 1;
    }
}
```

Read its setup in this order:

1. `HttpServer(port, "127.0.0.1", options)` creates the listener immediately.
2. Each `get`/`post` registers a function. It does not send a request.
3. `HttpServerRequest` gives that function the incoming data.
4. Returning a `HttpResponse` defines the response sent back to that client.
5. `HttpRouteOptions` describes the endpoint for Swagger. It does not run the
   handler or automatically implement application validation.
6. `enableSwagger` enables the documentation endpoints.
7. `serve()` runs the accept/handle loop until closed or cancelled.

The included operations are:

| Endpoint | Behavior |
| --- | --- |
| `GET /health` | Return `{"status":"ok"}`. |
| `GET /products` | Return two fixed sample products. |
| `GET /products/{id}` | Return product 1/2, or 404 with a JSON error. |
| `POST /echo` | Return the received body using its submitted Content-Type. |

The product data is a demonstration, not a database. Echo returns bytes without
parsing/validating JSON or saving anything. Real endpoints can replace these
functions with application logic and their chosen storage/JSON implementation.

## Complete client for that API

**File: `rest_client_example.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc > 3) {
        std::cerr<<"Usage: rest_client_example [base-url] [trusted-ca.pem]\n";
        return 1;
    }
    try {
        std::string base = argc >= 2 ? argv[1] : "http://127.0.0.1:8080";
        while (!base.empty() && base.back() == '/') base.pop_back();
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(5);
        if (argc == 3) options.caFile = argv[2];
        ncl::HttpClient client;
        const auto products = client.get(base + "/products", options);
        std::cout<<"GET /products: "<<products.statusCode<<'\n'<<products.body<<'\n';
        const auto product = client.get(base + "/products/1", options);
        std::cout<<"GET /products/1: "<<product.statusCode<<'\n'<<product.body<<'\n';
        const std::string content = R"({"message":"Hello from NCL"})";
        const auto echo = client.post(base + "/echo", content, "application/json", options);
        std::cout<<"POST /echo: "<<echo.statusCode<<'\n'<<echo.body<<std::endl;
        if (products.statusCode != 200 || product.statusCode != 200 || echo.statusCode != 200 || echo.body != content)
            return 1;
    } catch (const std::exception& error) {
        std::cerr<<"Error: "<<error.what()<<'\n';
        return 1;
    }
}
```

Here `client.get` actually sends a request. It differs from `server.get`, which
registers server behavior. Each client call returns only after the response
has been read completely, an error occurs, or its wait options interrupt it.

The client has no dependency on Swagger. Both speak ordinary HTTP to the same
server. Other HTTP clients, including a .NET client, can also call these paths.

## Run the API and try Swagger

Compile the two saved files separately on Windows:

```powershell
g++ -std=c++17 -Wall -Wextra -Wpedantic rest_server_example.cpp -o rest_server_example.exe -lws2_32
g++ -std=c++17 -Wall -Wextra -Wpedantic rest_client_example.cpp -o rest_client_example.exe -lws2_32
```

In terminal 1:

```powershell
.\rest_server_example.exe
```

Leave that terminal running. In terminal 2, in the same directory:

```powershell
.\rest_client_example.exe
```

The client prints successful GET/POST responses and terminates. The server
continues accepting requests. Stop it with Ctrl+C. On Linux use the same
compilation commands without `-lws2_32` and run `./rest_server_example` and
`./rest_client_example`. MSVC uses the earlier compiler flags for each source.

With the server running, open [Swagger](http://127.0.0.1:8080/swagger):

1. Expand `GET /products/{id}` and press **Try it out**.
2. Enter `1` in the required ID field, then press **Execute**.
3. Read status 200, response headers and `{"id":1,"name":"Keyboard"}`.
4. Try another unknown ID to see a 404 response from the same handler.
5. Expand `POST /echo`, activate Try it out, edit its JSON body and execute.

For example, submit `{"message":"Hello"}`; the response contains the same
content. The browser sends a real request to the local server. Swagger does
not simulate the handler or secretly call a separate implementation.

To use a different port:

```powershell
.\rest_server_example.exe 8081
.\rest_client_example.exe http://127.0.0.1:8081
```

Port 0 chooses a free port and prints its actual address. Use that printed
port in the client/browser. Changing a source requires stopping the old server,
recompiling and starting the new executable; existing processes do not reload
C++ source automatically.

## Create and inspect client requests

### GET and HEAD

For an existing client and configured `options`:

```cpp
auto response = client.get("http://127.0.0.1:8080/products", options);
std::cout<<response.statusCode<<' '<<response.reasonPhrase<<'\n';
std::cout<<response.body<<'\n';

auto metadata = client.head("http://127.0.0.1:8080/products", options);
auto contentType = metadata.headers.get("Content-Type");
```

HEAD reads response metadata without a message body. A Content-Length in a
HEAD response can describe the representation a GET would have returned; it
does not tell NCL to read that many body bytes from the HEAD response.

### POST and body types

```cpp
const std::string json = R"({"message":"Hello"})";
auto response = client.post("http://127.0.0.1:8080/echo", json, "application/json", options);
```

The third argument is the content type; options are fourth. There is no
`post(url, body, options)` overload in this API. Omitting the content type uses
`application/octet-stream`. Passing a JSON string does not parse it or infer
Content-Type. Embedded zero bytes are included when present in a string's length.

### PUT, PATCH, DELETE and custom headers

Use `HttpRequest` for methods/custom metadata beyond the convenience helpers:

```cpp
ncl::HttpRequest request;
request.method = "PUT";
request.headers.set("Content-Type", "text/plain");
request.headers.set("X-Request-ID", "123");
request.body = "Updated content";
auto response = client.request("http://127.0.0.1:8081/notes/1", request, options);
```

This fragment targets the notes example later, not the fixed products API.
Set `method` to PATCH or DELETE when calling corresponding endpoints. Methods
are case-sensitive tokens. CONNECT tunnels and protocol upgrades are unsupported.
The default request is GET with empty headers/body.

A response has `version`, integer `statusCode`, `reasonPhrase`, `headers`,
binary-safe `body` and separate `trailers`. Check `statusCode` before assuming
success. Read headers using `get`, accounting for a missing optional value:

```cpp
if (response.statusCode == 201) {
    if (auto location = response.headers.get("Location")) {
        std::cout<<"Created at "<<*location<<'\n';
    }
}
```

## Header rules and message bodies

`HttpHeaders` preserves field order and duplicate fields. Names use ASCII
case-insensitive comparison: Content-Type and content-type are the same name.
Values are not arbitrarily lowercased or combined. This matters for repeated
Set-Cookie fields and other fields with their own semantics.

| Operation | Behavior |
| --- | --- |
| `add(name, value)` | Append a field, preserving duplicates. |
| `set(name, value)` | Replace all matching fields with the new field. |
| `erase(name)` | Remove all matching fields. |
| `get(name)` | Optional copy of the first value. |
| `getAll(name)` | Vector containing every matching value. |
| `contains(name)` | Check for at least one matching field. |
| `size()` | Count fields, including duplicates. |
| Const iteration | Read `(name, value)` pairs in order. |

Header names must be HTTP tokens. Values reject CR, LF, NUL and other forbidden
control characters, preventing injected extra header lines. Horizontal tabs
are permitted. `add`/`set` reject invalid fields with
`HttpErrorKind::InvalidArgument` before changing the collection. Binary content
belongs in the body. Parsed values have leading/trailing spaces and tabs
removed; values added by application code retain their supplied whitespace.
Here a token means one or more ASCII letters, digits or characters from
``!#$%&'*+-.^_`|~``; it cannot contain spaces, colons or non-ASCII bytes.
The same token rule applies to method names and route-parameter names.

The client generates Host, Connection: close and the appropriate Content-Length.
User-supplied Host, Content-Length, Transfer-Encoding, Connection, Expect,
Upgrade, Trailer and TE are rejected because NCL manages them or does not
implement the required behavior. On the server, response Content-Length,
Transfer-Encoding, Connection, Trailer and Upgrade are managed/rejected; a
default Date is generated unless the response already supplies one.

Response reading supports fixed lengths, chunked transfer with bounded
extensions/trailers, and connection-close delimitation. Informational responses
are skipped up to a limit. HEAD, 204 and 304 are handled without a body; 205
content is rejected. Ambiguous/repeated lengths, bare LF, obsolete folding,
unsupported transfer codings and incomplete messages are rejected.

Server request bodies require Content-Length or supported chunked framing.
Without either, the request body is empty; closing the connection does not
delimit a request body. Received trailers stay in `trailers`, separate from
`headers`. Host, Content-Length, Transfer-Encoding, Connection, Trailer and
Upgrade are forbidden in trailers. Outgoing trailers are unsupported.

NCL buffers the complete content up to its configured limit; it has no streaming
body API. Its client asks for `Accept-Encoding: identity` unless overridden.
If a server nevertheless sends compressed content, bytes stay compressed and
Content-Encoding is preserved. There is no automatic decompression.

## Define server routes and response functions

The `HttpServer` constructor binds immediately. Defaults are port 8080 and
loopback `127.0.0.1`. Its full constructor is
`HttpServer(port, bindAddress, options, backlog, mode)`, with default options,
backlog 16 and IPv6-only mode when an IPv6 binding is chosen.

Register routes before serving. `get`, `post`, `put` and `patch` accept a path,
handler and optional documentation. `remove` registers DELETE, because delete
is a C++ keyword. `route(method, path, handler, docs)` also registers HEAD and
OPTIONS. Supported registration methods are GET, HEAD, POST, PUT, PATCH, DELETE,
OPTIONS, spelled in uppercase.

Route paths start with `/`, contain no query/fragment and follow the same
decoding rules as incoming paths. A template parameter occupies a full segment,
for example `{id}`. Its name must be a non-empty HTTP token without braces,
must be unique within that route and must not use percent escapes. There is
no wildcard/catch-all syntax or route-unregistration method. An empty callable,
unsupported method, invalid path or conflicting route is rejected with
`HttpErrorKind::InvalidArgument` during registration.

An endpoint function has this form:

```cpp
server.get("/health", [](const ncl::HttpServerRequest&) {
    return ncl::HttpResponse::json(R"({"status":"ok"})");
});
```

The unnamed request argument means this endpoint does not need to inspect
incoming data. For an endpoint that does, name the argument `request`. Return
a response rather than manually writing a status line to a socket.

### Response helpers

| Helper | Result |
| --- | --- |
| `HttpResponse::text(content, status = 200)` | Body plus `text/plain; charset=utf-8`. |
| `HttpResponse::json(content, status = 200)` | Body plus `application/json`; content must already be valid JSON. |
| `HttpResponse::empty(status = 204)` | Empty body with the chosen status. |

You can also set `statusCode`, `reasonPhrase`, `headers` and `body` on a response
yourself. The server supplies a common reason phrase when omitted; an unknown
valid status can have an empty phrase. It chooses the response protocol version
from the supported request version. HTTP statuses below 200, invalid status
numbers, invalid header values and unsupported response framing are rejected.
Valid server statuses are integers from 200 to 599. A returned response's
`version` field does not override the request-selected version. Its `trailers`
must be empty; a non-empty collection is rejected rather than transmitted.

Return 201 when your application has created a resource and optionally include
its location. Return 204 with no body when an operation succeeded without
content. Statuses 204, 205 and 304 cannot carry application response content in
this implementation. HEAD suppresses the body while otherwise following the
chosen response status: it normally reports representation length, while 204
and 304 have no generated Content-Length. A HEAD handler still constructs its
response body, and that body is subject to the configured size cap.

`HttpResponse::json` is a labeling/construction helper, not a JSON parser or
object serializer. NCL does not automatically turn structs into JSON, validate
JSON models, store records or generate database IDs. Those are application
responsibilities. The complete notes example below uses text/plain so its
storage behavior is visible without introducing a separate JSON dependency.

### Handler lifetime and mutable state

The server owns the registered callable. A handler can capture application
objects by reference if those objects outlive serving. The current server
invokes handlers sequentially. A mutable value captured inside a callable
persists across its calls; NCL does not reset it for every request.

Changing configuration while serving is unsupported. Calls to
route registration, enableTls or enableSwagger from a handler are rejected.
Wait until `serve()`/`serveOnce()` has returned before reconfiguring. A handler
can call `server.close()` on the owning thread; its already accepted response
can still finish before the loop exits. Closing there does not permit
reconfiguration before that serving call returns. Calling either serving
method recursively from a handler is also rejected.

## Read path, query, headers and body

`HttpServerRequest` extends ordinary request data with parsed addressing:

| Field/accessor | Meaning |
| --- | --- |
| `method` | Incoming case-sensitive method token. |
| `version` | HTTP/1.0 or HTTP/1.1. |
| `target` | Original request target, such as `/products/1?details=yes`. |
| `path` | Decoded path without query. |
| `query` | Query text without `?`, before decoding individual pairs. |
| `headers` | Incoming HTTP fields. |
| `body` | Complete binary-safe request content. |
| `trailers` | Separate trailing fields from a chunked request. |
| `remote` | Numeric peer Endpoint including address, port, family and scope. |
| `pathSegments` | Decoded path segments used by routing. |
| `pathParameters` | Ordered captured `(name, value)` pairs. |
| `queryParameters` | Ordered decoded query pairs, preserving repeated keys. |
| `pathParam(name)` | Optional first captured value with that name. |
| `queryParam(name)` | Optional first decoded query value. |
| `queryParams(name)` | Vector of all query values with that name. |

For a registered `/products/{id}`, `/products/1` captures `id = "1"`. A parameter
occupies one complete, non-empty segment. It is a string; validate/convert it
before using it as an integer. Parameter names must not be percent-encoded.
Method/path matching is case-sensitive and trailing slashes are significant.

A query such as `?name=Mario+Rossi&tag=a&tag=b` produces a name with a space
and two tag values. Read defaults deliberately instead of dereferencing a
missing optional:

```cpp
const auto name = request.queryParam("name").value_or("visitor");
const auto tags = request.queryParams("tag");
const auto contentType = request.headers.get("Content-Type");
const std::string& content = request.body;
```

Parameter-name lookup is case-sensitive. Query pairs are split on `&` and the
first `=` in each pair; empty pairs are ignored. `?flag` and `?flag=` both
produce a present `flag` with an empty string value. A missing key instead
returns `std::nullopt` from `queryParam` or an empty vector from `queryParams`.
Percent-encoded separators such as `%26` are part of a decoded value and do
not create extra pairs.

Percent escapes are decoded once. Query `+` means space; path `+` stays literal.
Encoded slash/backslash inside a path segment, dot segments, control characters,
fragments and malformed escapes are rejected. Decoding does not make arbitrary
input safe as a filesystem path, SQL statement or JSON string; how you use
application data remains your decision.

Concrete routes take precedence over parameter routes. If `/products/special`
exists only for POST, a GET there returns 405 rather than falling back to a
GET `/products/{id}` handler. Duplicate routes, equivalent parameter templates
with different names, and equally specific overlapping templates are rejected.
This keeps routing and generated OpenAPI descriptions consistent.

Unknown paths return 404. Known paths with no allowed method return 405 and
Allow. HEAD falls back to GET unless explicitly registered. OPTIONS is generated
unless explicitly registered; `OPTIONS *` describes available methods globally.
HEAD fallback invokes the actual GET handler, including any application side
effects, then suppresses its response body. Register an explicit HEAD handler
with `route("HEAD", ...)` when it needs different application behavior.

## Build a small create-read-update-delete API

This second server demonstrates actual changing state. It stores notes in a
map owned by the application. Bodies are text/plain; responses are text IDs,
note text or empty success. No external JSON parser/database is needed.

| Request | Application behavior |
| --- | --- |
| `POST /notes` | Create a note, return 201 with its ID and Location. |
| `GET /notes` | List existing IDs, one per line. |
| `GET /notes/{id}` | Read a note or return 404. |
| `PUT /notes/{id}` | Replace an existing note's text. |
| `PATCH /notes/{id}` | Append submitted text in this particular application. |
| `DELETE /notes/{id}` | Remove the note, return 204, or 404 if absent. |

PATCH semantics are chosen by this application; NCL does not implement JSON
Patch or JSON Merge Patch. Data exists only in memory and disappears when
the process stops. Persistent storage would be a separate application feature.

**File: `http_notes_server.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>
#include <map>

ncl::HttpRouteOptions describe(const std::string& summary, bool body, int status) {
    ncl::HttpRouteOptions docs;
    docs.summary = summary;
    docs.responseContentType = "text/plain";
    docs.successStatus = status;
    if (body) {
        docs.requestContentType = "text/plain";
        docs.requestBodyRequired = true;
    }
    return docs;
}

int main() {
    try {
        std::map<std::string, std::string> notes;
        unsigned nextId = 1;
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(5);
        options.maxBodyBytes = 64 * 1024;
        ncl::HttpServer server(8081, "127.0.0.1", options);

        server.post("/notes", [&](const ncl::HttpServerRequest& request) {
            if (request.body.empty()) return ncl::HttpResponse::text("Text is required", 400);
            const auto id = std::to_string(nextId++);
            notes.emplace(id, request.body);
            auto response = ncl::HttpResponse::text(id, 201);
            response.headers.set("Location", "/notes/" + id);
            return response;
        }, describe("Create a note", true, 201));

        server.get("/notes", [&](const ncl::HttpServerRequest&) {
            std::string ids;
            for (const auto& note : notes) ids += note.first + '\n';
            return ncl::HttpResponse::text(ids);
        }, describe("List note IDs", false, 200));

        server.get("/notes/{id}", [&](const ncl::HttpServerRequest& request) {
            const auto found = notes.find(*request.pathParam("id"));
            if (found == notes.end()) return ncl::HttpResponse::text("Not found", 404);
            return ncl::HttpResponse::text(found->second);
        }, describe("Read a note", false, 200));

        server.put("/notes/{id}", [&](const ncl::HttpServerRequest& request) {
            const auto found = notes.find(*request.pathParam("id"));
            if (found == notes.end()) return ncl::HttpResponse::text("Not found", 404);
            found->second = request.body;
            return ncl::HttpResponse::text(found->second);
        }, describe("Replace a note", true, 200));

        server.patch("/notes/{id}", [&](const ncl::HttpServerRequest& request) {
            const auto found = notes.find(*request.pathParam("id"));
            if (found == notes.end()) return ncl::HttpResponse::text("Not found", 404);
            if (request.body.size() > options.maxBodyBytes - found->second.size())
                return ncl::HttpResponse::text("Result would be too large", 413);
            found->second += request.body;
            return ncl::HttpResponse::text(found->second);
        }, describe("Append text to a note", true, 200));

        server.remove("/notes/{id}", [&](const ncl::HttpServerRequest& request) {
            if (notes.erase(*request.pathParam("id")) == 0)
                return ncl::HttpResponse::text("Not found", 404);
            return ncl::HttpResponse::empty();
        }, describe("Delete a note", false, 204));

        server.enableSwagger("Notes API", "1.0.0");
        std::cout<<"Notes API: http://127.0.0.1:8081/swagger"<<std::endl;
        server.serve();
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
```

The map is declared before the server, so the referenced application data
outlives all handlers during serving. Each callback returns a response for its
own request. The append handler enforces a limit on the resulting stored note,
not just on the incoming body. This is application-level validation that a
transport library cannot infer automatically.

Here is a client that exercises the whole lifecycle:

**File: `http_notes_client.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>

int main() {
    try {
        const std::string base = "http://127.0.0.1:8081";
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(5);
        ncl::HttpClient client;
        const auto created = client.post(base + "/notes", "First text", "text/plain", options);
        if (created.statusCode != 201) throw std::runtime_error("Creation failed");
        const auto location = created.headers.get("Location");
        if (!location) throw std::runtime_error("Creation response omitted Location");
        const auto url = base + *location;
        std::cout<<"Created "<<*location<<'\n';

        const auto original = client.get(url, options);
        if (original.statusCode != 200 || original.body != "First text")
            throw std::runtime_error("Reading the new note failed");

        ncl::HttpRequest update;
        update.method = "PUT";
        update.headers.set("Content-Type", "text/plain");
        update.body = "Replaced";
        if (client.request(url, update, options).statusCode != 200)
            throw std::runtime_error("Replacement failed");

        update.method = "PATCH";
        update.body = " and extended";
        if (client.request(url, update, options).statusCode != 200)
            throw std::runtime_error("Append failed");
        const auto changed = client.get(url, options);
        if (changed.statusCode != 200 || changed.body != "Replaced and extended")
            throw std::runtime_error("Updated content is wrong");
        std::cout<<changed.body<<'\n';

        ncl::HttpRequest remove;
        remove.method = "DELETE";
        if (client.request(url, remove, options).statusCode != 204)
            throw std::runtime_error("Deletion failed");
        if (client.get(url, options).statusCode != 404)
            throw std::runtime_error("Deleted note still exists");
        std::cout<<"Deleted; subsequent GET returned 404\n";
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
```

Compile both filenames with the earlier platform commands. Start the notes
server in one terminal, then the notes client in another. Expected output
includes `Replaced and extended` and the final 404 check. Alternatively open
[the notes Swagger page](http://127.0.0.1:8081/swagger), create a note with POST,
copy its returned ID, then test GET/PUT/PATCH/DELETE with that ID. Swagger marks
text/plain bodies as editable strings. Restarting the server clears all notes.

## Describe endpoints for Swagger

`enableSwagger(title, version, assetsBase)` serves the UI at `/swagger`,
`/swagger/` and `/swagger/index.html`, and its description at `/openapi.json`.
These paths are reserved when enabled; configuration conflicts are rejected.
`openApi()` also returns the description as a JSON string without requiring
the documentation endpoints to be enabled.

`HttpRouteOptions` is metadata copied at registration time:

| Field | Default and use |
| --- | --- |
| `summary` | Empty; operation description shown in Swagger. |
| `requestContentType` | Empty; set a media type to describe an editable body. |
| `responseContentType` | application/json; set the type your handler returns. |
| `successStatus` | 200; documented successful status, not an automatic handler result. |
| `requestBodyRequired` | false; whether the described request body is required. |
| `queryParameters` | Empty vector; optional string query fields displayed by Swagger. |

Path parameters are automatically described as required strings. To show query
fields, set their names in `queryParameters` before registering the route.
Metadata does not validate incoming requests or change response headers/status.
Keep it consistent with your handler. JSON body schemas are unrestricted and
other media types use string schemas; full object models, schema-based runtime
validation and automatic C++ object binding are not implemented.
An unrestricted JSON schema is selected only for the exact media type
`application/json` or a media type ending in `+json`, compared case-sensitively.
Other strings, including ones with appended media-type parameters, receive
a string schema. This choice affects documentation only.

The first three fields are `std::string`, `successStatus` is `int`,
`requestBodyRequired` is `bool`, and `queryParameters` is
`std::vector<std::string>`. Metadata strings must be valid UTF-8. A documented
status outside 200..599, empty query name or repeated query name is rejected
at registration. `requestBodyRequired` only appears in OpenAPI when
`requestContentType` is non-empty. Descriptions include explicitly registered
operations; generated HEAD/OPTIONS behavior is not added as extra operations.

The description uses OpenAPI 3.0.3 with a relative server root, allowing the
browser to use the host/port it opened. Swagger calls the real handlers on the
same origin, so this UI does not need an automatically permissive CORS policy.
An unrelated browser frontend on another origin needs an application-defined
CORS policy/appropriate endpoint responses; NCL does not configure one for you.

### Swagger assets and offline operation

By default, the page loads Swagger UI 5.34.1 JS/CSS from
`https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.34.1`. The browser therefore
needs Internet access for the interface. HTTP endpoints, the generated JSON
description and C++ server/client do not require that CDN or a Swagger build
dependency. The page disables the external specification validator and does
not persist authorization settings.

For offline UI assets, obtain the official Swagger UI distribution and serve
its `swagger-ui.css` and `swagger-ui-bundle.js` through application routes or
a reverse proxy. Pass their base path, for example `/swagger-assets`, as the
third enableSwagger argument. NCL does not automatically read/serve a folder
from disk. Supplying a local path alone does not make the files available.
Title, version and asset base must be non-empty. The asset base must be an
ASCII HTTP(S) URL or an absolute local URL path starting with a single `/`;
protocol-relative `//host/path` bases are rejected. Re-enabling Swagger before
serving replaces its settings; there is no disable method.

## URLs and addresses

Client URLs must be absolute `http://` or `https://` ASCII URLs with a non-empty
host and optional decimal port from 1 to 65535. Defaults are 80 and 443.
Empty paths become `/`. Query parameters are retained; fragments are not sent.
Path percent escapes are checked and sent without decoding by the client.
Encode spaces/non-ASCII path text first. Raw Unicode hostnames/IDNA, URL
credentials, IPv6 zone identifiers and proxies are not supported here.

```cpp
auto local = client.get("http://127.0.0.1:8080/health", options);
auto ipv6 = client.get("http://[::1]:8080/health", options);
```

IPv6 brackets are required inside a URL. The server's constructor takes a
separate bind address, so use `"::1"` there without brackets. `HttpOptions.family`
filters TCP destination resolution; default Unspecified tries resolver order.
One first-address failure may use the shared budget before another family is
attempted. Numeric loopback avoids DNS lookup for local introductory examples.

The default HttpServer binds IPv4 loopback. To expose it on your LAN choose
`"0.0.0.0"` explicitly and connect using the server computer's real IPv4 address.
Never use `0.0.0.0` as the intended remote destination. Loopback is local to the
computer running the client/browser. Allow the TCP application/port through
the relevant private-network firewall if needed; NCL does not modify it.
The constructor's port is `std::uint16_t`: validate external integers before
converting them. Local port zero selects a free port; a client URL's destination
port must instead be from 1 to 65535.

For an IPv6-only server use a binding such as `HttpServer(8080, "::1", options)`.
For both IP families on one all-interface binding:

```cpp
ncl::HttpServer both(8080, "::", options, 16, ncl::IPv6Mode::DualStack);
```

The listener follows [ServerSocket IPv6 rules](TCP.md#addresses-ports-ipv6-and-lan-use).
Dual stack requires an IPv6 binding and cannot create network connectivity
that the host/network does not have.

## Configure limits and operation budgets

| HttpOptions field | Default | Meaning |
| --- | --- | --- |
| `wait` | Empty WaitOptions | Timeout and cancellation; zero timeout has no NCL deadline. |
| `maxHeaderBytes` | 32768 | Each request/response header section including start line and CRLF; each informational section has its own cap. Also caps trailers. |
| `maxBodyBytes` | 8388608 | Body byte cap for either direction. Complete bodies are buffered. |
| `maxChunkMetadataBytes` | 262144 | Total chunk-size lines, extensions, delimiters and trailers. |
| `maxInformationalResponses` | 8 | Client cap on informational responses before the final response. |
| `caFile` | Empty | Client PEM trust bundle; empty uses supported system trust anchors. |
| `family` | Unspecified | Client TCP resolver family filter. Server binding chooses its listener family. |

For example:

```cpp
ncl::HttpOptions options;
options.wait.timeout = std::chrono::seconds(5);
options.maxBodyBytes = 2 * 1024 * 1024;
```

Limits count bytes, not Unicode characters. Set enough header capacity for
generated Host/Connection/length fields as well as your custom headers. A body
cap applies to outgoing bodies too. Lowering an incoming limit does not make
a larger body stream to a file: streaming is not implemented.
The four size/count limits have type `std::size_t`. Zero is a literal cap,
not an unlimited sentinel: a zero body cap permits only empty bodies, a zero
informational-response cap permits none, and zero header capacity cannot fit
a normal HTTP message. `wait` is `WaitOptions`, `caFile` is `std::string` and
`family` is `AddressFamily`.

A client call shares one deadline across connection attempts, TLS negotiation,
request headers/body, response headers/body and the TLS closing notification.
It does not restart between reads or after receiving headers. Each new client
call gets a new budget. Portable DNS remains synchronous and outside it, with
cancellation checked before and after resolution. A time budget is not a
real-time guarantee against OS scheduling or arbitrary synchronous application work.

For HttpServer, the request budget starts **after accept**. It covers TLS,
reading the request, executing the handler and writing the response. Handlers
run synchronously; checks before and after them cannot forcibly interrupt a
handler blocked in unrelated application code. Configure a positive request
timeout to bound idle network clients.

`serveOnce(acceptWait)` gives the accept stage its own wait options; its timeout
does not become the accepted request's timeout. A non-empty accept cancellation
token also controls the accepted request and overrides the token in
`options.wait`. Without one, the request uses `options.wait.cancellation`.
The token in `options.wait` alone does not interrupt an idle accept.
`serve(token)` uses an unlimited accept timeout and that explicit token for
both stages. To stop an idle `serve`, pass a token to `serve` itself.
Client-only options such as family/caFile do not configure server TLS; the
server gets its binding in the constructor and certificate/key through enableTls.

## Cancel a running server safely

The following complete program waits until its controller requests a stop.
The server has one owning thread; the controller only changes the shared token.

**File: `http_cancel_server.cpp`**

```cpp
#include "NCL.hpp"
#include <iostream>
#include <thread>

int main() {
    try {
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(5);
        ncl::HttpServer server(0, "127.0.0.1", options);
        ncl::CancellationSource stop;
        std::thread controller([stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.requestCancellation();
        });
        try {
            server.serve(stop.token());
        } catch (...) {
            stop.requestCancellation();
            controller.join();
            throw;
        }
        controller.join();
        std::cout<<"Serving stopped; listener still open: "<<server.isOpen()<<'\n';
        server.close();
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
```

Compile with `-pthread` when using GCC. Cancellation makes `serve` return
normally and leaves listener ownership intact. When the server is owned by
a worker, request cancellation, join that worker, then close/move the server.
Do not concurrently close or reconfigure a server from a controller thread.

The same cancellation source/token can be placed in a client's options.wait.
Source copies share the one-way request; tokens keep it alive. Requests cannot
be reset. Socket waits check tokens at up to 20 ms intervals plus scheduling
delays, with synchronous DNS outside that observation. Completion can win a
race; cancellation does not undo an operation already processed by a server.

## Understand errors and retry policy

Separate these three results in your application:

| Result | What to inspect |
| --- | --- |
| Valid HTTP response, including 4xx/5xx | `response.statusCode`, headers and body. |
| Network failure | Catch `NetworkError`; inspect kind/native code. |
| HTTP validation/TLS failure | Catch `HttpError`; inspect its kind. |

For existing client/url/options variables:

```cpp
try {
    const auto response = client.get(url, options);
    if (response.statusCode >= 400) {
        std::cerr<<"Server replied "<<response.statusCode<<'\n';
    }
} catch (const ncl::HttpError& error) {
    std::cerr<<"HTTP/TLS failure: "<<error.what()<<'\n';
} catch (const ncl::NetworkError& error) {
    std::cerr<<"Network failure: "<<error.what()<<'\n';
}
```

`HttpError` derives from `std::runtime_error`. Its const/noexcept `kind()`
returns the scoped enum `HttpErrorKind`; inherited `what()` returns
`const char*`. It has no `nativeCode()` or `bytesTransferred()` accessor.

| HttpErrorKind | Meaning |
| --- | --- |
| `InvalidArgument` | Invalid method/header, server configuration, metadata or outgoing server response. |
| `InvalidUrl` | Unsupported URL scheme or invalid host, port, syntax or percent escape. |
| `InvalidResponse` | Malformed/incomplete response, ambiguous framing or excess informational responses. |
| `HeadersTooLarge` | Header, trailer or chunk-metadata byte cap exceeded. |
| `BodyTooLarge` | Incoming or outgoing body exceeds the configured cap. |
| `Unsupported` | Requested HTTP/TLS feature or transfer coding is unsupported, or TLS was not compiled in. |
| `Tls` | TLS setup, handshake, transport or closing failure other than a classified certificate failure. |
| `Certificate` | Client trust roots cannot be loaded or server certificate verification fails. |
| `InvalidRequest` | Malformed incoming request, target, Host or framing. |
| `UnsupportedVersion` | Incoming request version is not HTTP/1.0 or HTTP/1.1. |
| `ExpectationFailed` | Incoming request has an unsupported Expect header. |

`serveOnce` handles ordinary rejected client messages and sends the status
described below when possible. TLS server certificate/key loading problems
are reported during `enableTls`; their category can be `Tls`.

`NetworkError` provides kind(), nativeCode(), what() and bytesTransferred().
During HTTP, that progress count describes the failing underlying TCP transfer,
not the entire exchange or confirmed application processing. Standard allocation
exceptions retain their type.

The client owns a fresh connection for every request and releases it on success
or failure. It never returns an incomplete response as if complete. It does
not automatically retry or follow redirects. A failed POST might already have
changed server state even if its reply was lost; your application must decide
whether retrying is safe. `Location` is returned as a header, not automatically
requested with copied credentials.

### Server rejection behavior

| Condition | Usual response |
| --- | --- |
| Unknown path | 404. |
| Method not registered for the best matching path | 405 with Allow. |
| Malformed request, bad/missing/duplicate Host, ambiguous framing | 400. |
| Incoming body above cap | 413. |
| Header/chunk metadata cap exceeded | 431. |
| Expect header requiring unsupported behavior | 417. |
| Unsupported coding, upgrade or tunnel | 501. |
| Unsupported HTTP version | 505. |
| Handler throws or returns an invalid response | Generic 500, without private exception text. |

HTTP/1.1 requires one valid Host header; HTTP/1.0 may omit it. Incoming chunked
bodies/trailers are supported with limits. Competing Transfer-Encoding and
Content-Length or repeated lengths are rejected conservatively and the
connection is closed. Unsupported Expect is rejected before waiting for a
body that a client may be withholding for 100 Continue.

A disconnected/timed-out client can be closed without a response when the
remaining transport/budget cannot send one. Ordinary client failures release
that connection and leave the listener usable. Accept errors still propagate;
allocation failures are not silently converted into ordinary client failures.
`serveOnce` returns `void`; a normal return can mean a handler ran, a request
was rejected, or a disconnected/timed-out client was released. It does not
report the handler's status to its caller. Accept timeout/cancellation and
request cancellation propagate as `NetworkError`. `serve(token)` converts
`Cancelled` to a normal return only when its own supplied token is cancelled;
a request-only cancellation token in `options.wait` does not have that rule.

## Enable HTTPS in the build

HTTPS uses the same HTTP messages through TLS, adding confidentiality, integrity
and server identity verification. HTTP/1.1 over TLS normally uses port 443;
the local examples can choose 8443. Changing only the port does not enable TLS.

Define NCL_ENABLE_TLS before the **first** inclusion of NCL.hpp. Use that macro
consistently in every translation unit. NCL implements TLS internally; no
OpenSSL headers, libraries, DLLs or executable are required by your application.
Without the macro, supportsHttps() is false, HTTPS client URLs are rejected
before I/O and server enableTls reports Unsupported. TCP/UDP/plain HTTP remain usable.

The internal TLS implementation is experimental and has not undergone an
independent cryptographic audit. Read the [TLS guide](TLS.md) for the exact
supported algorithms, certificate profile and security/verification limits.

### GCC or MinGW on Windows

Use the same system Winsock link as plain HTTP:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -DNCL_ENABLE_TLS rest_server_example.cpp -o rest_server_example.exe -lws2_32
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -DNCL_ENABLE_TLS rest_client_example.cpp -o rest_client_example.exe -lws2_32
```

Optimization is recommended for the software cryptography. No extra crypto
library or DLL is supplied by the application. The same flag applies to the
other complete HTTP programs.

### GCC on Linux

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -DNCL_ENABLE_TLS rest_server_example.cpp -o rest_server_example
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -DNCL_ENABLE_TLS rest_client_example.cpp -o rest_client_example
```

The header uses the system's secure random generator and supported certificate
bundle locations. No separate TLS package is needed to compile these programs.

### MSVC and CMake

In a Visual Studio developer prompt, use `/DNCL_ENABLE_TLS` with `/std:c++17`
and preferably `/O2`. Winsock is linked by the header; no OpenSSL setup is needed.
The development CMake project enables the same internal feature with
`-DNCL_TEST_TLS=ON`, without finding or linking a third-party crypto library.

## HTTPS clients and trust configuration

Client HTTPS supports TLS 1.2/1.3 within the [documented profile](TLS.md#supported-protocol-profile),
certificate chain/validity checking, matching DNS-name or IP identity, DNS SNI
and HTTP/1.1 ALPN. There is no insecure
certificate-bypass flag and no automatic downgrade to HTTP.

For an already-created client:

```cpp
ncl::HttpOptions secure;
secure.wait.timeout = std::chrono::seconds(10);
secure.caFile = "trusted-ca.pem";
auto response = client.get("https://localhost:8443/health", secure);
```

When caFile is empty, Windows uses its ROOT certificate store and Linux uses
a supported system CA bundle. Default anchors are cached for the process;
restart it after changing that system set. An explicit caFile replaces the
default anchors and is read for each request. See [trust details and constraints](TLS.md#trust-stores-and-identity-checks).
Providing a trusted CA does not skip hostname matching or expiry checking.

For a localhost certificate, use an identity listed in that certificate's
Subject Alternative Name. Trusting a certificate for localhost does not make
it valid for an unrelated hostname or arbitrary LAN IP. An IP URL is checked
against IP certificate identities. Common Name fallback is not implemented;
the required DNS/IP identity must appear in Subject Alternative Name.

TLS uses NCL's own record/handshake engine over its timed socket transport. Connection, handshake,
data and the closing notification share the HTTP operation's budget. An
unauthenticated TCP EOF is rejected when EOF is needed to delimit the HTTPS
body. A complete length-delimited response does not require reading beyond
its body. Closing sends a notification and allows up to 50 ms, within the
remaining operation budget, to receive the peer's notification and avoid a
native reset. A complete framed body does not require that closing reply.
There is no general-purpose public TLS Socket API.

## HTTPS server and a local certificate

Configure a PEM certificate chain and matching unencrypted RSA PEM private key
before serving. The current server supports RSA 2048..4096-bit identities;
the client can also verify supported ECDSA certificates:

```cpp
ncl::HttpServer server(8443, "127.0.0.1", options);
server.enableTls("server.pem", "server.key");
```

The listener is now for HTTPS; sending ordinary plaintext HTTP to it is not
accepted as a fallback. Configuration validates the files/key match and throws
on failure. Paths must be non-empty and contain no NUL characters. The server
stores the filenames and reloads their contents for each new TLS connection;
keep those files available while serving. This server authenticates itself;
mutual TLS/client certificate authentication is not implemented. Application-level login/authorization is
a separate concern.

Certificate files can come from your chosen authority or separate tool. NCL
does not require that tool to build or run. For an isolated local exercise,
the following optional OpenSSL command makes a disposable
two-day self-signed certificate with localhost and loopback IP identities:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key -out server.pem -days 2 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1"
```

It writes files in the current directory. Keep a real private key private;
these disposable test files are not a production certificate setup. Build the
example pair with TLS enabled, then run separately on Windows:

```powershell
# Terminal 1
.\rest_server_example.exe 8443 server.pem server.key
# Terminal 2
.\rest_client_example.exe https://localhost:8443 server.pem
```

On Linux use `./rest_server_example 8443 server.pem server.key` and
`./rest_client_example https://localhost:8443 server.pem`. The client uses the
explicit self-signed certificate as its test trust file. This does not change
the operating system's or browser's trust store.

A browser opening HTTPS Swagger must independently trust a valid server
certificate. NCL's caFile setting affects only the NCL client, not your browser.
Use an appropriate trusted development/real certificate for browser HTTPS;
the HTTP loopback Swagger exercise earlier needs no certificate configuration.

## Server concurrency, ownership and scope

> [!IMPORTANT]
> The current server handles **one connection at a time**, with one request per
> connection and no internal worker pool. A slow request/handler can delay others.
> Each accepted request is fully buffered within configured caps. Neither
> persistent connections nor streaming bodies are implemented.

Use one owning thread per HttpServer. Its listener resource cannot be copied;
move it only when quiescent, with application captures/lifetimes still valid.
Configure routes/TLS/Swagger before serving. For another thread to stop serving,
use cancellation and join, not concurrent close or move. Different independent
servers can have different owners.

The library provides transport, routing and documentation building blocks.
It does not automatically provide a database, dependency injection, background
jobs, authentication middleware, object/model binding, CORS policy, HTTP cache,
cookie jar, decompression, redirects or retries. HTTP/2, HTTP/3, CONNECT,
WebSocket/protocol upgrades and outgoing chunked bodies are outside this version.
Applications can manually supply authorization headers and handle them in routes.

## Public API reference

### HttpClient

`HttpClient` is default-constructible. URL parameters are `const std::string&`;
`request` takes `const HttpRequest&`, and `post` takes its `std::string` body
and content type by value. Options are `HttpOptions` passed by value. All four
request methods are const and return `HttpResponse` by value. They retain no
reference to caller-owned request data after returning.

| Method | Result |
| --- | --- |
| `supportsHttps()` | Static constexpr/noexcept `bool` reflecting NCL_ENABLE_TLS. |
| `get(url, options = {})` | Send GET and return HttpResponse. |
| `head(url, options = {})` | Send HEAD and return response metadata/empty body. |
| `post(url, body, contentType = "application/octet-stream", options = {})` | Send POST with a body/content type. |
| `request(url, request, options = {})` | Send a custom HttpRequest and return its HttpResponse. |

The client is synchronous and holds no reusable connection state; each call
creates/releases its own connection. A URL must be a complete HTTP(S) URL.

### HttpServer

`HttpServer::Handler` is
`std::function<HttpResponse(const HttpServerRequest&)>`. The request reference
is valid only during that handler invocation; copy any data you need to retain.
Registration takes method/path strings, handler and `HttpRouteOptions` by value
and stores the handler/metadata. Route registration, TLS/Swagger configuration
and serving methods return `void`; `openApi()` is const and returns `std::string`.
Port is `std::uint16_t`, bind address is `const std::string&`, backlog is `int`
and mode is `IPv6Mode`. The constructor takes `HttpOptions` by value.
`serve` takes `CancellationToken`; `serveOnce` takes `WaitOptions`, both by value.

| Method | Result |
| --- | --- |
| `HttpServer(port = 8080, bindAddress = "127.0.0.1", options = {}, backlog = 16, mode = IPv6Mode::IPv6Only)` | Create the listener; port zero chooses a free port. |
| `route(method, path, handler, docs = {})` | Register one supported method/path. |
| `get`, `post`, `put`, `patch` | Convenience `(path, handler, docs = {})` registration. |
| `remove(path, handler, docs = {})` | Register DELETE. |
| `enableSwagger(title = "NCL API", version = "1.0.0", assetsBase = "https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.34.1")` | Enable documentation routes before serving. |
| `openApi()` | Return generated OpenAPI JSON. |
| `enableTls(certificateFile, privateKeyFile)` | Configure validated server certificate/key before serving. |
| `serve(cancellation = {})` | Serve until closed or the supplied token is cancelled. That token's cancellation returns normally, leaving listener owned. |
| `serveOnce(acceptWait = {})` | Accept and handle one connection. |
| `localEndpoint()` | Const method returning an `Endpoint` with binding/family/actual port; requires an open listener. |
| `isOpen()` | Const/noexcept method returning `bool` for listener ownership. |
| `close()` | Release the listener; idempotent, noexcept, owner thread only. |

Backlog controls a pending-connection queue request, not the number of worker
threads. A closed server does not offer a rebind method; construct a replacement.
TLS certificate/key and Swagger arguments are `std::string` values. The server's
destructor releases its listener; move construction/assignment are noexcept,
while copying is deleted.

### Request and response values

| Type | Fields/defaults |
| --- | --- |
| `HttpRequest` | method GET, empty HttpHeaders and empty body. |
| `HttpResponse` | Empty version/reason/headers/body/trailers; statusCode 0 until set or returned by a client/helper. |
| `HttpServerRequest` | HttpRequest data plus version, target, path, query, trailers, remote, path/query pairs and pathSegments. |
| `HttpRouteOptions` | Metadata defaults documented in [Describe endpoints for Swagger](#describe-endpoints-for-swagger). |
| `HttpOptions` | Waits/limits/TLS trust/family defaults in [Configure limits and operation budgets](#configure-limits-and-operation-budgets). |
| `HttpHeaders` | Ordered `(std::string, std::string)` fields; initializer-list construction, add/set/erase/get/getAll/contains/size and const iteration. |
| `HttpError` | std::runtime_error with kind() returning HttpErrorKind. |

The public exception constructor is `HttpError(kind, message)`, taking
`HttpErrorKind` and `const std::string&`. NetworkError's constructor/diagnostics
are covered in the [TCP reference](TCP.md#shared-value-types) and
[error section](TCP.md#errors-progress-and-retry-decisions).

Request/response textual fields and bodies are `std::string`; `statusCode`
is `int`, header/trailer collections are `HttpHeaders`, and `remote` is
`Endpoint`. Path/query pair vectors contain
`std::pair<std::string, std::string>`; `pathSegments` is
`std::vector<std::string>`. The three parameter accessors are const and take
`std::string_view` names: `pathParam`/`queryParam` return
`std::optional<std::string>` copies, and `queryParams` returns
`std::vector<std::string>`.
Default construction leaves the server request's extra strings/collections
empty and `remote` as a default `Endpoint`; inherited `method` starts as GET.
The server fills these fields before invoking a handler. The three static
response helpers take content by `std::string` value where applicable, an
`int` status, and return `HttpResponse` by value.

`HttpHeaders::Field` aliases `std::pair<std::string, std::string>`.
Construction is empty or from `std::initializer_list<Field>`. `add`/`set`
take `std::string` name/value arguments by value and return `void`; `erase`
takes `std::string_view` and returns `void`. Const lookups take
`std::string_view`: `get` returns `std::optional<std::string>`, `getAll`
returns `std::vector<std::string>`, and `contains` returns `bool`. Const/noexcept
`size()` returns `std::size_t`; const/noexcept `begin()`/`end()` support read-only
iteration. Modify fields through `add`/`set`/`erase`, not through iterators.
Collection changes can invalidate iterators; lookup results are independent copies.

Response helper signatures and incoming accessors are described above. Shared
Endpoint, WaitOptions, AddressFamily and cancellation types are documented in
[TCP](TCP.md#shared-value-types); HTTP's binding/family rules are explained here.
The public request/response/header values can be copied as ordinary data.

## Troubleshoot an API

| Symptom | Check |
| --- | --- |
| Connection refused | Start the server; confirm port, address and IP family. |
| Server prints its address and appears to stop | It is normally waiting in serve; use another terminal/browser to call it. |
| Route change not visible | Stop the old server, recompile, restart, then refresh Swagger. |
| 404 | Verify exact path/case/trailing slash and parameter segment structure. |
| 405 | Call the method actually registered for that path; inspect Allow. |
| 500 | Check handler exceptions and invalid status/body/managed response headers. |
| 413 or 431 | Check the corresponding body/header/metadata limits before increasing them. |
| POST body not editable in Swagger | Set requestContentType in route metadata before registration. |
| Swagger operation description differs from response | Metadata describes behavior; it does not set the handler's actual result. |
| Swagger page is blank offline | Its default JS/CSS use a CDN; provide/serve local assets or restore access. |
| API returns JSON but no C++ object appears | Parse the body in application code using your chosen JSON implementation. |
| Another computer cannot connect | Loopback is local; choose an appropriate binding, real server address and TCP firewall/network configuration. |
| Certificate error | Check trust roots, expiry, DNS/IP identity, and that the server is actually TLS-enabled. |
| HTTPS fails only in browser | Browser trust is independent of NCL caFile. |
| Server stalls during a handler | Network timeouts cannot preempt arbitrary synchronous application work. |
| HTTPS reports Unsupported | Define NCL_ENABLE_TLS consistently before including the header. No SSL/Crypto link flags are needed. |
| Valid-looking certificate is rejected | Check the supported key/signature algorithms, critical/path constraints, SAN and trust bundle in the TLS guide. |

## Verification and protocol references

Current internal TLS checks are recorded in [TLS verification](../test/TLS_VERIFICA.md),
including independent HTTPS peers, primitive vectors, malformed input, certificate
constraints and sanitizer runs. Ordinary HTTP tests include framing, limits,
interruption, binary data and IPv6. Swagger was also exercised through browser
Try it out GET and POST calls during the preceding endpoint-server work.

These checks do not establish Internet-wide interoperability, physical LAN
behavior, 32-bit compatibility, independent security assurance or sustained-load
performance. See the current TLS record and the historical
[HTTP verification](../test/HTTP_VERIFICA.md) and
[server verification](../test/SERVER_VERIFICA.md) for recorded development results.

Primary references:

- [HTTP semantics](https://www.rfc-editor.org/rfc/rfc9110.html).
- [HTTP/1.1 message format](https://www.rfc-editor.org/rfc/rfc9112.html).
- [Built-in TLS profile and specifications](TLS.md).
- [OpenAPI 3.0.3](https://spec.openapis.org/oas/v3.0.3).
- [Swagger UI installation](https://swagger.io/docs/open-source-tools/swagger-ui/usage/installation/).

Return to the [README](../README.md), [TCP guide](TCP.md) or [UDP guide](UDP.md).
