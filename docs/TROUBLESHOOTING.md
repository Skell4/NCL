# Troubleshooting NCL

![Troubleshooting](../assets/docs/help.svg)

[Home](../README.md) | [Start here](GETTING_STARTED.md) | [Server tutorial](SERVER.md) | [API reference](API.md) | [Build](BUILDING.md)

> [!TIP]
> Start with the matching symptom below. Keep the error kind and native code
> when checking a network failure; they help distinguish its cause.

## Build problems

| Symptom | Check and next step |
| --- | --- |
| `NCL.hpp` not found | Save the program beside the exact uppercase header name, or add its directory to the include path. |
| `g++` or `cl` not found | Select/install the intended compiler; for MSVC use a Visual Studio developer prompt. |
| Missing `std::optional`, `string_view` or inline-variable support | Enable C++17 or later. |
| Multiple definitions of `main` | Compile each complete **File** example as a separate executable. |
| Undefined `WSA...`, `socket` or `getaddrinfo` symbols on Windows | GCC/MinGW needs `-lws2_32` after sources/objects. MSVC uses the header's link instruction. |
| Linux cannot find `ws2_32` | Remove the Windows-only link flag. |
| Winsock type/redefinition errors | Avoid including old `winsock.h`; include Winsock2/NCL before Windows headers that introduce Winsock1. |
| Program uses threads but will not link/run correctly | Use `-pthread` in GCC application builds, or CMake `Threads::Threads`. |
| HTTPS reports `Unsupported` | Rebuild every relevant source with `NCL_ENABLE_TLS` defined before the first include. |
| Looking for `NCL_IMPLEMENTATION` or OpenSSL linker flags | There is no implementation macro and no external crypto link dependency. See [build setup](BUILDING.md). |
| Strict checks fail with MSVC C4702 in a TLS-disabled Release build | Code following the unavailable-TLS path is diagnosed as unreachable. The current plain-mode verification uses `/wd4702`; TLS is still unavailable. See [the review record](../test/DOCS_REFRESH_VERIFICA.md). |
| MinGW 15.2 stops with `-Werror=maybe-uninitialized` at optimized `WaitContext` code | This diagnostic was encountered in the strict TLS example build. The MSVC TLS build passed unchanged; see [recorded compiler observations](../test/DOCS_REFRESH_VERIFICA.md#compiler-observations) before treating the MinGW strict run as a pass. |

## Connections and server lifecycle

| Symptom | Check and next step |
| --- | --- |
| Server prints its address and then waits | `serve` is waiting normally; use a second terminal/browser to make requests. |
| Connection refused | Check that the server is running and that scheme, port, bind address and family match. |
| Address/port already in use | Stop your earlier instance or choose another port; do not terminate an unrelated service. Use listener port 0 when a fixed port is unnecessary. |
| Second computer cannot connect | Loopback addresses are local. Bind the intended interface and use the server's actual LAN address; check the private-network firewall. |
| Using `0.0.0.0` as a URL | It is a wildcard binding; use a real destination address instead. |
| IPv6 URL rejected | Use brackets, for example `http://[::1]:8080/health`; constructor bind addresses use `::1` without brackets. |
| DNS exceeds the timeout | Resolution is synchronous and outside the socket timeout. Use numeric addresses for bounded local tests. |
| One slow request delays all others | This server is sequential. Set a request budget; backlog does not add workers. |
| Timeout cannot stop a handler | NCL checks before/after synchronous handlers; your application must bound its own work. |
| Cancellation does not stop an idle server | Pass the token to `serve(token)`; `options.wait.cancellation` alone does not control accept. |
| `managed_server` exits when launched without a console | Input EOF requests cancellation. Run it interactively or integrate a service-specific stop mechanism. |
| `isConnected()` is true after the peer exited | It reports local handle state, not a live peer check. Observe reads, EOF and errors. |

## Routes, bodies and Swagger

| Symptom | Check and next step |
| --- | --- |
| 404 | Check exact path/case/trailing slash and whether the route was registered before serving. |
| 405 | Check the HTTP method and `Allow`; a more specific literal path can take precedence over a template. |
| 400 | Inspect application validation and HTTP framing, including Host and conflicting lengths. |
| 413 / 431 | Body / header-or-metadata limit exceeded. Confirm units and payload sizes before increasing caps. |
| 415 in the tutorial echo route | Send exactly one `Content-Type: text/plain` header. That example deliberately rejects media-type parameters. |
| 500 | Check handler exceptions and invalid outgoing statuses/headers/body rules; log details privately in application code. |
| JSON is not parsed into a C++ object | Bodies are strings. `HttpResponse::json` labels already-valid JSON; use application parsing/serialization as needed. |
| HEAD handler has side effects | HEAD falls back to GET. Keep GET safe or register explicit HEAD behavior with `route`. |
| Swagger has no editable body | Set route `requestContentType` metadata before registering the route. |
| Metadata changed but behavior did not | Metadata describes handlers; it does not implement validation or set actual status/content type. Recompile/restart after code changes. |
| Blank Swagger page offline | Serve local Swagger JS/CSS and pass their base URL; NCL does not automatically serve a disk directory. |
| Browser frontend fails while curl works | Check the actual browser error and origin/CORS policy; NCL does not configure permissive CORS. |

## TLS and certificates

Start with [the HTTPS walkthrough](SERVER.md#step-6-enable-https) and the
[supported TLS profile](TLS.md#supported-protocol-profile).

| Symptom | Check and next step |
| --- | --- |
| HTTPS to a plain HTTP listener fails | Rebuild with TLS, call `enableTls` before serving and use the listener's HTTPS port. |
| Plain HTTP to a TLS listener fails | This is expected; there is no plaintext fallback. |
| Untrusted local self-signed certificate | Supply it explicitly as the exercise's `caFile`, or use a certificate issued by an appropriate trusted CA. |
| Trust works but hostname still fails | The URL's DNS name/IP must match a SAN; changing a CA file does not bypass identity checks. |
| Certificate has a Common Name but no SAN | CN fallback is not implemented. Obtain a certificate with the required SAN identities. |
| Expired/not-yet-valid certificate | Check certificate dates and the computer's clock; replace/renew the certificate. |
| Browser rejects HTTPS but NCL accepts it | Browser trust is independent of NCL's per-request CA file. Configure a properly trusted development certificate for that browser. |
| Certificate valid elsewhere but rejected by NCL | Check supported keys/signatures, chain order, intermediates, critical extensions and path constraints in [TLS](TLS.md). |
| EC or encrypted private key rejected by the server | Current server keys are unencrypted RSA 2048..4096-bit PKCS#1/PKCS#8 PEM. EC verification support does not imply EC server-key support. |
| Certificate/key worked initially, later requests fail | Files are reloaded on each connection. Check working directory, file permissions, availability and matching replacements. |
| System trust changes have no effect | Default anchors are process-cached; restart the process. An explicit CA file is loaded per request. |
| Peer uses only another cipher suite/group | It is outside the supported TLS profile; NCL does not silently weaken verification. |
| Searching for a verify-false switch | No insecure certificate-bypass option is exposed. Fix trust, identity, time or the supported profile. |

## TCP and UDP specifics

`receiveMessage` expects NCL's length prefix; it cannot read HTTP, arbitrary
newline text or Java `writeUTF` directly. An empty message is valid; `nullopt`
means clean EOF before the next frame. Partial frame failure can close the
connection, so inspect [TCP error and retry behavior](TCP.md#errors-progress-and-retry-decisions).

An empty UDP packet is data, not EOF. `DatagramTruncated` means the packet was
consumed and discarded because the buffer was too small. UDP itself does not
guarantee delivery, order or uniqueness; see [UDP limits](UDP.md#capacity-truncation-and-payload-limits).

For reproducible library issues, record the compiler/platform, whether TLS is
enabled, the relevant options and a minimal program. Preserve error kinds,
native codes and transferred-byte counts without including private keys or
credentials. [Build/check instructions](BUILDING.md#development-checks-in-this-folder)
explain how to rerun the documented examples.
