# NCL built-in TLS guide

![Tls / Https](../assets/docs/security.svg)

[Home](../README.md) | [Start here](GETTING_STARTED.md) | [Server tutorial](SERVER.md) | [API reference](API.md) | [Build](BUILDING.md)

NCL contains its own TLS implementation in `NCL.hpp`. HTTPS applications need
no OpenSSL headers, libraries, DLLs or executable. Enable it by defining
`NCL_ENABLE_TLS` consistently in every translation unit. Applications use
`HttpClient`, `HttpServer`, `HttpOptions` and `enableTls`; the TLS engine is
internal. Start with the [HTTPS server walkthrough](SERVER.md#step-6-enable-https)
for a complete, small example and client.

> [!WARNING]
> **This is experimental cryptographic software.** Known-answer vectors,
> independent interoperability checks and sanitizer runs have been performed;
> there has been no independent cryptographic audit, security certification or
> production-hardening assessment. This guide states the supported profile and
> its limits. A successful HTTPS example is not evidence that every attack or
> side channel has been excluded.

Save each complete **File** program next to `NCL.hpp` in your application folder;
run build commands there. The `docs/` directory contains the guides, not the
working directory for compilation. [Build instructions](BUILDING.md).

## Contents

- [Build without external libraries](#build-without-external-libraries)
- [How HTTPS fits together](#how-https-fits-together)
- [Supported protocol profile](#supported-protocol-profile)
- [Certificates and server keys](#certificates-and-server-keys)
- [Trust stores and identity checks](#trust-stores-and-identity-checks)
- [Handshake and record behavior](#handshake-and-record-behavior)
- [Timeouts and cancellation](#timeouts-and-cancellation)
- [Cryptographic implementation](#cryptographic-implementation)
- [Current limits](#current-limits)
- [Development verification](#development-verification)
- [Primary specifications](#primary-specifications)

## Build without external libraries

Copy just `NCL.hpp` for the library. Save the complete server/client programs
from the [HTTP guide](HTTP.md#complete-endpoint-server) next to it. No generated
TLS source files or development folders need to accompany the header.

Windows with GCC/MinGW:

```powershell
g++ -std=c++17 -O2 -DNCL_ENABLE_TLS rest_server_example.cpp -o rest_server_example.exe -lws2_32
g++ -std=c++17 -O2 -DNCL_ENABLE_TLS rest_client_example.cpp -o rest_client_example.exe -lws2_32
```

Windows with MSVC, from a Visual Studio developer command prompt:

```bat
cl /EHsc /std:c++17 /O2 /W4 /utf-8 /DNCL_ENABLE_TLS rest_server_example.cpp /Fe:rest_server_example.exe
cl /EHsc /std:c++17 /O2 /W4 /utf-8 /DNCL_ENABLE_TLS rest_client_example.cpp /Fe:rest_client_example.exe
```

Linux:

```sh
g++ -std=c++17 -O2 -DNCL_ENABLE_TLS rest_server_example.cpp -o rest_server_example
g++ -std=c++17 -O2 -DNCL_ENABLE_TLS rest_client_example.cpp -o rest_client_example
```

Optimization is recommended for the software cryptography. Add `-pthread` to
GCC programs that create application threads. Windows uses its normal system
Winsock library; MSVC links it through the header. A MinGW executable can still
use the standard C++/compiler runtime DLLs selected by that toolchain. There
are no third-party cryptographic dependencies.

Without `NCL_ENABLE_TLS`, `HttpClient::supportsHttps()` returns false,
HTTPS URLs fail with `HttpErrorKind::Unsupported` before connection I/O and
`HttpServer::enableTls` reports Unsupported. TCP, UDP and plain HTTP remain usable.

## How HTTPS fits together

The public API remains HTTP. TLS runs underneath it:

1. The client opens a TCP connection and offers supported TLS parameters.
2. Client and server select a version, cipher suite and ephemeral exchange group.
3. The server supplies its certificate chain and proves possession of its key.
4. The client checks that chain against its trust anchors, the requested SAN
   identity, validity times and NCL's supported certificate constraints.
5. Both peers derive traffic keys and verify the handshake's Finished messages.
6. HTTP headers and bodies travel in authenticated, encrypted TLS records.
7. TLS closing notifications distinguish authenticated closure from bare TCP EOF.

This is a conceptual outline, not a wire-level handshake transcript; TLS 1.2
and TLS 1.3 differ in their message/key schedule. The primary protocol
specifications are [RFC 8446](https://www.rfc-editor.org/rfc/rfc8446.html) and
[RFC 5246](https://www.rfc-editor.org/rfc/rfc5246.html). Service identity is
described in [RFC 9525](https://www.rfc-editor.org/rfc/rfc9525.html); NCL's
implemented subset and exclusions are listed below.

Certificate trust and a matching identity are separate requirements. Encrypting
to an unverified peer is insufficient to establish the intended server's
identity. HTTPS also does not log in an application user or authorize an action;
those belong in your application.

## Supported protocol profile

| Feature | Client | Server |
| --- | --- | --- |
| TLS 1.3 | `TLS_AES_128_GCM_SHA256` (`0x1301`). | Same suite. |
| TLS 1.2 | `TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256` (`0xc02f`) or `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256` (`0xc02b`). | RSA suite `0xc02f`. |
| Ephemeral key exchange | X25519, secp256r1/P-256, secp384r1/P-384. | Same groups. |
| TLS 1.3 server signature | RSA-PSS/SHA-256 with an ordinary RSA key; ECDSA P-256/SHA-256 or P-384/SHA-384. | RSA-PSS/SHA-256. |
| TLS 1.2 server signature | RSA-PSS/SHA-256, RSA PKCS#1/SHA-256, ECDSA/SHA-256 or SHA-384. | RSA-PSS/SHA-256, or PKCS#1/SHA-256 when offered. |
| Certificate public keys | RSA 2048..4096 bits; ECDSA P-256/P-384. | Server leaf/key must be RSA 2048..4096 bits. |
| Certificate signatures | RSA PKCS#1 with SHA-256/384/512; RSA-PSS with SHA-256/MGF1-SHA-256 and a 32-byte salt; ECDSA with SHA-256/384/512 using a supported issuer curve. | The loaded chain must use supported public-key formats. Clients decide whether to trust it. |
| HTTP protocol | HTTP/1.1 ALPN; HTTP/1.0 response parsing remains available. | HTTP/1.1 ALPN when offered. |

TLS 1.2 requires the extended-master-secret extension. SSL and TLS 1.0/1.1,
CBC, static RSA key exchange, anonymous suites and compression are rejected.
The client prefers TLS 1.3; it can negotiate TLS 1.2 with a compatible peer.
Version downgrade sentinels and selected versions/suites/shares are checked.

These are the suites and algorithms implemented, not a claim to replace the
complete OpenSSL product or to support every valid TLS deployment. A peer
configured with only other algorithms cannot connect through this profile.

## Certificates and server keys

`server.enableTls(certificateFile, privateKeyFile)` loads a PEM certificate
chain and an **unencrypted RSA PEM private key**. Supported key containers are
`RSA PRIVATE KEY` (two-prime PKCS#1) and `PRIVATE KEY` (PKCS#8 with rsaEncryption).
Encrypted keys, RSA-PSS-only key containers, multi-prime RSA, EC server private
keys and automatic certificate/key generation are outside this version.

Put the leaf certificate first, followed by required intermediate certificates.
The chain is limited to eight entries, each at most 128 KiB. Normally omit the
root from the served chain; the client must obtain trust independently.
Configuration parses the files and checks the leaf/key match. RSA private
operations verify their result with the public key before releasing a signature.

The server stores filenames and reloads them for each TLS connection. Keep
both files accessible while serving. Replacing a file can change subsequent
connections; a partially written or invalid replacement fails that connection.
NCL does not obtain, renew or install certificates, and does not alter the OS
or browser trust stores.

Certificate files are input data. A certificate-authority service or a separate
tool can produce them; NCL does not require that tool at build time or runtime.
The HTTP guide's OpenSSL command is an optional way to create a disposable
local test certificate, not an NCL application dependency.

## Trust stores and identity checks

`HttpOptions::caFile` selects a PEM bundle of explicitly trusted certificates.
An explicit file replaces the default anchor set for that request. It can also
contain a deliberately trusted self-signed leaf for a local exercise. Merely
receiving a self-signed certificate from a server never makes it trusted.

When `caFile` is empty:

- Windows reads the current user's `ROOT` certificate store using the system
  Crypt32 service. NCL performs the supported certificate validation itself.
- Linux reads the first available standard bundle from
  `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt` or
  `/etc/ssl/ca-bundle.pem`.

Default anchors are cached for the process. Restart it after changing that
system trust set. An explicit `caFile` is read for each request. Unsupported or
malformed root formats do not become anchors; if none are supported, configure
a supported PEM bundle. NCL does not download missing intermediate certificates.

Validation checks signatures, validity times, issuer/subject links, supported
critical extensions, CA/basic constraints, path-length constraints, signing key
usage and server-authentication extended usage. Issuer/subject linking uses
their encoded DER names. Duplicate extensions, malformed DER and unsupported
critical extensions are rejected. Name constraints, policy mappings/constraints
and inhibit-any-policy are conservatively rejected rather than ignored.

The requested host must match **Subject Alternative Name**. DNS comparison is
ASCII case-insensitive; a supported wildcard is a whole leftmost label, such
as `*.example.com`, matching one label only. It does not match deeper names or
a bare suffix. IP URLs require an exact binary IP SAN. Common Name fallback,
raw international hostnames and automatic IDNA conversion are not implemented.
Normal certificate verification cannot be disabled through an insecure flag.

There is no CRL/OCSP checking, AIA fetching or certificate-transparency policy.
A chain passing this supported profile has not thereby been checked for
revocation or every possible PKIX policy.

## Handshake and record behavior

NCL implements the TLS handshake, transcript hashing, Finished verification,
record encryption/authentication and alert handling itself. It does not invoke
SChannel, WinHTTP, another TLS provider or a bundled third-party TLS engine.

TLS 1.3 handles HelloRetryRequest with supported alternate groups, validates
the second ClientHello and supports a retry cookie on the client. A server
retry with a PSK offer is outside this profile. An optional client-certificate
request receives an empty certificate list; there is no client-certificate
authentication API. A peer requiring a client certificate will reject it.

TLS 1.3 KeyUpdate is processed in both directions, including requested replies.
Tickets can be received and discarded; they are not stored for resumption.
Handshake ordering, extension duplication, lengths, selected parameters and
changes of encryption keys are checked before application data is returned.

TLS plaintext records are at most 16 KiB. The ciphertext cap is 16 KiB plus
256 bytes; a handshake message/transcript is capped at 1 MiB. Certificate
entry and chain limits apply independently. Excessive compatibility records,
empty records or tickets are rejected. Traffic keys have a conservative
2^24-record limit. TLS 1.3 updates its write key before reaching it; exhausting
a receive key or a TLS 1.2 key ends the connection instead of reusing nonces.

Application plaintext is returned only after the GCM tag verifies. Modified
records and Finished/signature mismatches fail the connection. A TCP EOF is
not authenticated TLS EOF: `close_notify` is required when EOF delimits an
HTTPS body. A complete Content-Length/chunked body does not require reading
beyond its framing. NCL sends its close notification and allows a closing
grace of up to 50 ms, capped by the remaining operation budget, to consume
the peer's notification before native socket close. Missing closure after a
fully framed HTTP body does not become authenticated EOF or incomplete content.

## Timeouts and cancellation

The same `HttpOptions::wait` budget covers connection attempts, TLS handshake,
certificate/signature work, HTTP data and closing notification. Network waits
and long modular-arithmetic loops check that context. A timeout is not reset
for every record, key change, certificate or handshake message.

DNS remains synchronous and outside the socket budget. Synchronous filesystem
and OS trust-store operations cannot be forcibly interrupted; elapsed time is
observed at the next check. `enableTls` configuration itself has no wait-options
argument. Handlers and other application code remain synchronous as described
in the HTTP guide. Cancellation does not roll back requests already processed.

## Cryptographic implementation

The header implements SHA-256/384/512, HMAC-SHA-256, HKDF, the TLS 1.2 PRF,
AES-128/GCM, X25519, RSA signing/verification and the supported EC operations.
AES avoids secret-indexed S-box tables. RSA signing uses exponent-blinded CRT
and verifies its output. Private EC multiplication uses a fixed operation
schedule and masked selection; public ECDSA verification uses a separate path.
Internal heap buffers for bytes/large integers are erased before deallocation.
These implementation choices are not a verified constant-time or complete
memory-erasure guarantee across compilers/hardware.

Fresh random bytes come only from the operating system: BCryptGenRandom on
Windows and getrandom on Linux. Failure aborts the operation; there is no
`rand`, clock-based seed or predictable fallback. Windows system services are
loaded from the system directory; no additional crypto import libraries need
to be supplied to the application linker.

## Current limits

There is no TLS resumption, PSK, 0-RTT, TLS 1.2 renegotiation, mutual TLS,
Ed25519/Ed448, P-521, post-quantum/hybrid exchange, other cipher suites or public
general-purpose TLS socket API. Unsupported parameters fail instead of silently
disabling authentication or switching to plaintext HTTP.

The HTTP server remains sequential, handling one request per connection.
There is no internal worker pool, and software cryptography adds CPU cost.
The supported systems remain Windows and Linux. Platform, compiler, load and
physical-network coverage is stated in the verification record.

## Development verification

See [the internal TLS verification record](../test/TLS_VERIFICA.md) for actual
commands, counts, compiler results, malformed-input checks and remaining limits.
Development code under `test/tls_source/` is amalgamated into the single header;
applications do not include or ship those files.

OpenSSL/Python SSL are used as independent test peers and to generate local
test certificates. The separate `tls_openssl_peer` reference executable links
OpenSSL deliberately; NCL application executables do not. Published NIST and
RFC vectors test results against independent expected values. Passing those
vectors is not NIST validation or a FIPS certificate.

## Primary specifications

- [TLS 1.3, RFC 8446](https://www.rfc-editor.org/rfc/rfc8446.html).
- [TLS 1.2, RFC 5246](https://www.rfc-editor.org/rfc/rfc5246.html).
- [TLS AES-GCM suites, RFC 5288](https://www.rfc-editor.org/rfc/rfc5288.html).
- [ECDHE AES-GCM suites, RFC 5289](https://www.rfc-editor.org/rfc/rfc5289.html).
- [Extended master secret, RFC 7627](https://www.rfc-editor.org/rfc/rfc7627.html).
- [X.509 path validation, RFC 5280](https://www.rfc-editor.org/rfc/rfc5280.html).
- [RSA encodings, RFC 8017](https://www.rfc-editor.org/rfc/rfc8017.html).
- [X25519, RFC 7748](https://www.rfc-editor.org/rfc/rfc7748.html).
- [HKDF, RFC 5869](https://www.rfc-editor.org/rfc/rfc5869.html).
- [EC parameters/vectors, RFC 5903](https://www.rfc-editor.org/rfc/rfc5903.html).
- [ECDSA known answers, RFC 6979](https://www.rfc-editor.org/rfc/rfc6979.html).
- [NIST GCM vector source](https://csrc.nist.gov/projects/cryptographic-algorithm-validation-program/cavp-testing-block-cipher-modes).
