# Build an HTTP and HTTPS server step by step

![Step By Step](../assets/docs/tutorial.svg)

[Home](../README.md) | [Start here](GETTING_STARTED.md) | **Server tutorial** | [API reference](API.md) | [Build](BUILDING.md)

Start with [the small hello server](GETTING_STARTED.md). This lesson expands
it into a small API with explicit limits, validated input, Swagger, HTTPS and
an orderly stop. Each stage explains what the application must supply.

> [!TIP]
> Follow the steps in order: start with local HTTP, test the routes, then add
> certificates and HTTPS. Each **File** block is a complete program.

## Contents

- [Step 1: decide the server contract](#step-1-decide-the-server-contract)
- [Step 2: write a complete API](#step-2-write-a-complete-api)
- [Step 3: compile and test plain HTTP](#step-3-compile-and-test-plain-http)
- [Step 4: understand validation and responses](#step-4-understand-validation-and-responses)
- [Step 5: try Swagger](#step-5-try-swagger)
- [Step 6: enable HTTPS](#step-6-enable-https)
- [Step 7: write an HTTPS client](#step-7-write-an-https-client)
- [Step 8: stop the server cleanly](#step-8-stop-the-server-cleanly)
- [Step 9: move from loopback to a real service](#step-9-move-from-loopback-to-a-real-service)
- [Step 10: verify your application](#step-10-verify-your-application)

## Step 1: decide the server contract

This API uses IPv4 loopback. It has three routes:

| Request | Result |
| --- | --- |
| `GET /health` | `200`, JSON `{"status":"ok"}`. |
| `GET /greet/{name}?style=normal` | `200`, plain-text greeting. `style=short` returns only the name. |
| `POST /echo` with `Content-Type: text/plain` | `200`, the supplied non-empty text. |

Names accept 1..40 ASCII letters, digits, `_` or `-`. The echo route accepts
at most 1024 bytes. Unknown styles, repeated style values or empty bodies
return 400; another content type returns 415. The server-wide body cap is
64 KiB; requests above it are rejected before the handler, normally with 413.
If a rejected body is already being transmitted, native connection closure
can interrupt delivery of that error response; the client must handle failure.

Keeping the first mutable-data exercise separate makes each step small.
For persistent-in-process create/read/update/delete operations, continue with
the [notes API](HTTP.md#build-a-small-create-read-update-delete-api).

## Step 2: write a complete API

Save this program next to `NCL.hpp`. It accepts **no arguments** for HTTP on
8080, or **two filenames** for HTTPS on 8443. The same handlers work in both.

**File: `api_server.cpp`**

```cpp
#include "NCL.hpp"
#include <chrono>
#include <exception>
#include <iostream>
#include <string>

bool validName(const std::string& name) {
    if (name.empty() || name.size() > 40) return false;
    for (unsigned char ch : name) {
        const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool digit = ch >= '0' && ch <= '9';
        if (!letter && !digit && ch != '_' && ch != '-') return false;
    }
    return true;
}

ncl::HttpResponse noStore(ncl::HttpResponse response) {
    response.headers.set("Cache-Control", "no-store");
    response.headers.set("X-Content-Type-Options", "nosniff");
    return response;
}

int main(int argc, char* argv[]) {
    if (argc != 1 && argc != 3) {
        std::cerr << "Usage: api_server [certificate.pem private.key]\n";
        return 1;
    }
    try {
        const bool secure = argc == 3;
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(10);
        options.maxHeaderBytes = 16 * 1024;
        options.maxBodyBytes = 64 * 1024;
        options.maxChunkMetadataBytes = 16 * 1024;

        ncl::HttpServer server(secure ? 8443 : 8080, "127.0.0.1", options);
        if (secure) server.enableTls(argv[1], argv[2]);

        ncl::HttpRouteOptions healthDocs;
        healthDocs.summary = "Check whether the API is responding";
        server.get("/health", [](const ncl::HttpServerRequest&) {
            return noStore(ncl::HttpResponse::json(R"({"status":"ok"})"));
        }, healthDocs);

        ncl::HttpRouteOptions greetDocs;
        greetDocs.summary = "Greet a validated name";
        greetDocs.responseContentType = "text/plain";
        greetDocs.queryParameters = {"style"};
        server.get("/greet/{name}", [](const ncl::HttpServerRequest& request) {
            const auto name = request.pathParam("name").value_or("");
            const auto style = request.queryParam("style").value_or("normal");
            if (!validName(name) || request.queryParams("style").size() > 1 ||
                (style != "normal" && style != "short")) {
                return noStore(ncl::HttpResponse::text("Invalid name or style", 400));
            }
            return noStore(ncl::HttpResponse::text(
                style == "short" ? name : "Hello, " + name + "!"));
        }, greetDocs);

        ncl::HttpRouteOptions echoDocs;
        echoDocs.summary = "Echo 1..1024 bytes of plain text";
        echoDocs.requestContentType = "text/plain";
        echoDocs.responseContentType = "text/plain";
        echoDocs.requestBodyRequired = true;
        server.post("/echo", [](const ncl::HttpServerRequest& request) {
            const auto types = request.headers.getAll("Content-Type");
            if (types.size() != 1 || types.front() != "text/plain") {
                return noStore(ncl::HttpResponse::text("Use Content-Type: text/plain", 415));
            }
            if (request.body.empty() || request.body.size() > 1024) {
                return noStore(ncl::HttpResponse::text("Body must be 1..1024 bytes", 400));
            }
            return noStore(ncl::HttpResponse::text(request.body));
        }, echoDocs);

        server.enableSwagger("NCL tutorial API", "1.0.0");
        std::cout << "API: " << (secure ? "https" : "http")
                  << "://127.0.0.1:" << server.localEndpoint().port << std::endl;
        server.serve();
    } catch (const ncl::HttpError& error) {
        std::cerr << "HTTP/TLS setup or protocol error: " << error.what() << '\n';
        return 1;
    } catch (const ncl::NetworkError& error) {
        std::cerr << "Network error: " << error.what()
                  << " (native code " << error.nativeCode() << ")\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

The named response function adds two application headers to these handlers'
responses. It is ordinary C++, not framework middleware; built-in error and
Swagger responses do not pass through it. The constant health JSON is already
valid JSON. User text is returned as text, without concatenating it into JSON.

## Step 3: compile and test plain HTTP

Windows GCC/MinGW:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic api_server.cpp -o api_server.exe -lws2_32
.\api_server.exe
```

Linux:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic api_server.cpp -o api_server
./api_server
```

For MSVC use [the equivalent build command](BUILDING.md#msvc-on-windows).
In another terminal, run these requests. On Windows spell the command
`curl.exe` if PowerShell defines a `curl` alias.

```sh
curl --include http://127.0.0.1:8080/health
curl --include "http://127.0.0.1:8080/greet/Sage?style=short"
curl --include -H "Content-Type: text/plain" --data-binary "Hello" http://127.0.0.1:8080/echo
curl --include "http://127.0.0.1:8080/greet/Sage?style=unknown"
curl --include -H "Content-Type: application/json" --data-binary "{}" http://127.0.0.1:8080/echo
```

Expect statuses 200, 200, 200, 400 and 415. The first three bodies are
`{"status":"ok"}`, `Sage` and `Hello`. The server continues waiting after each.
For these examples Ctrl+C terminates it; step 8 shows controlled cancellation.

## Step 4: understand validation and responses

NCL validates HTTP framing and route syntax. Your handler validates business
input. Path/query parameters are strings, even if a route calls one `{id}`.
Check ranges, missing values and repeated values before using them.

The echo handler deliberately accepts the exact header value `text/plain`.
It rejects parameters such as `text/plain; charset=utf-8`; a broader application
should parse media types and define its accepted parameters. Route metadata
only describes this contract; it does not enforce it.

Use 400 for malformed application input, 404 for a missing resource, 409 for
an application conflict and 201 when creating a resource. Return
`HttpResponse::empty()` for a 204 success. Do not put a body on 204, 205 or 304.
For additional headers, modify the returned response's `headers` collection.
NCL manages Host/connection/framing fields; see [header rules](HTTP.md#header-rules-and-message-bodies).

Handlers throwing ordinary exceptions receive a generic 500 response when the
connection can still carry one; exception text is not returned to the client.
Catch expected domain errors in your own handler and log useful diagnostics
there. The surrounding `main` catch is for errors that escape the serving API;
ordinary per-client failures may be handled internally. There is no built-in
request/error logging callback.

## Step 5: try Swagger

Open [Swagger on the running HTTP server](http://127.0.0.1:8080/swagger).
Expand `GET /greet/{name}`, click **Try it out**, supply `Sage`, optionally
set `style` to `short`, then execute. For `POST /echo`, enter plain text.
The description is also available at `/openapi.json`.

The default JavaScript/CSS come from a pinned CDN. For a disconnected machine,
serve local assets as described in [offline Swagger](HTTP.md#swagger-assets-and-offline-operation).
Swagger's routes are enabled without application authentication. If the API
requires access controls for documentation, omit `enableSwagger` and expose an
exported `openApi()` description through an appropriately protected service.
There is no global authentication middleware in this version.

## Step 6: enable HTTPS

> [!IMPORTANT]
> HTTPS is HTTP carried through TLS. NCL implements the transport itself;
> setting port 8443 alone does not activate it. The implementation's supported
> [TLS profile](TLS.md#supported-protocol-profile) is experimental and has not
> received an independent cryptographic audit.

1. Stop the plain HTTP server.
2. Rebuild `api_server.cpp` with `NCL_ENABLE_TLS` enabled consistently.
3. Obtain a PEM certificate chain and a matching unencrypted RSA PEM private key.
4. Pass the two filenames; `enableTls` validates them before serving.
5. Use an `https://` URL and a client that trusts the certificate's issuer/identity.

Windows GCC/MinGW:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -DNCL_ENABLE_TLS api_server.cpp -o api_server.exe -lws2_32
```

Linux:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -DNCL_ENABLE_TLS api_server.cpp -o api_server
```

For this **local exercise only**, an optional OpenSSL command can create a
disposable two-day self-signed RSA certificate. Run it in your application
folder; choose fresh filenames if these already exist:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout server.key -out server.pem -days 2 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1"
```

OpenSSL creates input files here; it is not an NCL build/runtime dependency.
NCL currently loads RSA 2048..4096-bit server identities in unencrypted PKCS#1
or PKCS#8 containers. A real certificate chain starts with the server leaf,
followed by needed intermediates. Protect the private key with OS file access
permissions. NCL does not obtain or renew certificates.

Run on Windows:

```powershell
.\api_server.exe server.pem server.key
curl.exe --cacert server.pem --include https://127.0.0.1:8443/health
```

On Linux use `./api_server server.pem server.key` and `curl --cacert ...`.
Expect a 200 response. The explicit trust file is for this self-signed local
exercise; ordinary authority-issued certificates use an appropriate CA bundle.

The requested DNS name or IP must appear in **Subject Alternative Name**.
Trusting a certificate does not make it valid for another host or LAN IP.
`caFile`/`--cacert` do not alter browser trust. HTTPS Swagger requires a certificate
your browser independently trusts; follow your browser/OS development-certificate
procedure. There is no certificate-bypass option in NCL.

The server reloads certificate/key filenames for each connection. Keep them
readable and replace them carefully. A stopped-server update followed by a
restart avoids readers observing a mismatched pair. See [certificate details](TLS.md#certificates-and-server-keys).

## Step 7: write an HTTPS client

This complete client takes a URL and an optional PEM trust file. It uses system
trust when the second argument is omitted. It checks the HTTP status as well
as transport success.

**File: `secure_client.cpp`**

```cpp
#include "NCL.hpp"
#include <chrono>
#include <exception>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: secure_client https://host/path [trusted-ca.pem]\n";
        return 1;
    }
    try {
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(10);
        options.maxBodyBytes = 64 * 1024;
        if (argc == 3) options.caFile = argv[2];
        ncl::HttpClient client;
        const auto response = client.get(argv[1], options);
        std::cout << response.statusCode << '\n' << response.body << '\n';
        return response.statusCode >= 200 && response.statusCode < 300 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Compile with the same TLS flag as the server, substituting `secure_client.cpp`
and the output name. For the local self-signed exercise on Windows:

```powershell
.\secure_client.exe https://127.0.0.1:8443/health server.pem
```

On Linux use `./secure_client https://127.0.0.1:8443/health server.pem`.
Expect `200` and `{"status":"ok"}`. This program also accepts plain HTTP URLs
because `HttpClient` supports both schemes; an application requiring HTTPS
should constrain the URLs it accepts. There is no automatic redirect or
plaintext downgrade. An explicit `caFile` replaces the system anchors for
that call. See [trust behavior](TLS.md#trust-stores-and-identity-checks).

## Step 8: stop the server cleanly

The thread doing socket I/O must own the server. Another thread requests
cancellation through a token; it must not concurrently call `close` or change
routes. Here the main thread reads the console, while the worker owns the
server. This separate HTTP program focuses only on lifecycle; it can use the
same routes/options/TLS configuration as the preceding example before the move.

**File: `managed_server.cpp`**

```cpp
#include "NCL.hpp"
#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

int main() {
    try {
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(5);
        options.maxBodyBytes = 64 * 1024;
        ncl::HttpServer server(8082, "127.0.0.1", options);
        server.get("/health", [](const ncl::HttpServerRequest&) {
            return ncl::HttpResponse::text("OK");
        });

        ncl::CancellationSource stop;
        std::exception_ptr failure;
        std::thread owner([owned = std::move(server), token = stop.token(), &failure]() mutable {
            try {
                owned.serve(token);
                owned.close();
            } catch (...) {
                failure = std::current_exception();
            }
        });

        std::cout << "API: http://127.0.0.1:8082/health" << std::endl;
        std::cout << "Press Enter to stop." << std::endl;
        std::string line;
        std::getline(std::cin, line);
        stop.requestCancellation();
        owner.join();
        if (failure) std::rethrow_exception(failure);
        std::cout << "Stopped cleanly.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Compile with `-pthread` in GCC/MinGW, plus Windows' `-lws2_32` when applicable:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread managed_server.cpp -o managed_server.exe -lws2_32
```

Linux:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread managed_server.cpp -o managed_server
```

Run it in an interactive terminal, call `/health`, then press Enter. Input EOF
also requests stopping. If serving fails early, press Enter to join the worker
and report its exception; this console example does not implement an OS service
supervisor. The worker alone writes `failure`; main reads it only after `join`.

`serve(token)` returns normally for cancellation of its supplied token. The
listener remains owned until closed/destroyed. Cancellation can interrupt an
in-progress request and its response; it does not drain all pending work or
undo state changes. Handlers doing unrelated blocking work must provide their
own stop/deadline behavior. Do not call NCL cancellation methods directly from
an asynchronous C signal handler; integrate the platform's service/signal
notification mechanism with a normal application thread.

## Step 9: move from loopback to a real service

Choose requirements before exposing the listener:

| Concern | What to configure or implement |
| --- | --- |
| LAN binding | Use the intended server interface or `0.0.0.0` for all IPv4 interfaces; clients use the server's actual address. |
| IPv6 | Bind `::1` for local IPv6; `::` plus `IPv6Mode::DualStack` for both families if supported by the host. |
| TLS identity | Obtain a supported certificate for the exact DNS/IP identities, ship intermediates, protect/renew keys. |
| Request size/time | Set positive time budgets and appropriate byte caps; add shorter domain limits in handlers. |
| Identity and permissions | Implement authentication and authorization for each sensitive operation; HTTPS alone identifies the server, not the user. |
| Browser frontend | Define an explicit CORS/CSRF/cookie policy if required; NCL does not supply one automatically. |
| Storage | Choose validation, durable storage and transaction rules. Tutorial in-memory data disappears at exit. |
| Logs | Record outcomes and useful errors in application code; avoid logging credentials, keys or sensitive bodies. |
| Concurrency/load | Measure your workload. This server is sequential; backlog is a pending queue, not a worker count. |
| Documentation access | Enable Swagger only where intended; its built-in routes have no authentication hook. |
| Operation | Define restart, health checks, time synchronization, certificate replacement and controlled shutdown. |

Configure a private-network firewall rule for the intended application/port
when needed; NCL does not change firewall settings. Do not use `0.0.0.0` as a
client URL host. An IPv4 loopback certificate does not cover arbitrary LAN IPs.

The experimental TLS engine's test record is not a production-security
assessment. A deployment involving sensitive data needs an appropriate
security review of the actual application and its transport. No configuration
in this guide removes the implementation limits in [TLS](TLS.md#current-limits)
or [HTTP server scope](HTTP.md#server-concurrency-ownership-and-scope).

## Step 10: verify your application

Before relying on a new route, check observable behavior:

| Test | Expected outcome |
| --- | --- |
| Valid request | Intended status, content type and body. |
| Missing/invalid/repeated inputs | Defined 4xx response, no unintended mutation. |
| Unknown path / wrong method | 404 / 405 with `Allow`. |
| Too-large header/body | Rejected within the configured limits, normally with 431 / 413 when the error response can be delivered. |
| Slow or disconnected client | Accepted connection released; next client can still connect. |
| Handler exception | Generic 500 where possible; useful private application diagnostics. |
| Untrusted, expired or wrong-name certificate | HTTPS client rejects it. |
| Plain HTTP sent to an HTTPS listener | No plaintext fallback. |
| Stop while idle and during network I/O | Cancellation returns and the owning thread can join. |
| OpenAPI and Swagger | Described methods/body types match the handlers' actual responses. |

The repository's [build/check instructions](BUILDING.md#development-checks-in-this-folder)
cover library examples. Add application tests for business rules, storage,
permissions and workload that those examples do not exercise.
