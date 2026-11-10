# Getting started with NCL

![Step By Step](../assets/docs/tutorial.svg)

[Home](../README.md) | **Start here** | [Server tutorial](SERVER.md) | [API reference](API.md) | [Build](BUILDING.md)

This exercise runs entirely on your computer. First start a small HTTP server;
then call it from a C++ client. HTTPS is added in the [server tutorial](SERVER.md).
No NCL installer or separate compiled library is needed.

## Step 1: prepare a folder

Create an application folder and copy `NCL.hpp` into it. Install or select a
C++17 compiler: GCC/MinGW on Windows, MSVC in a Visual Studio developer prompt,
or GCC on Linux. Python, OpenSSL and CMake are not needed for these two programs.

Your application folder will contain:

```text
my-app/
  NCL.hpp
  hello_server.cpp
  hello_client.cpp
```

> [!TIP]
> All commands below run from that application folder. Do not compile from
> `docs/` unless you explicitly supply the appropriate include/source paths.

## Step 2: write the server

**File: `hello_server.cpp`**

```cpp
#include "NCL.hpp"
#include <chrono>
#include <exception>
#include <iostream>

int main() {
    try {
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(5);
        options.maxBodyBytes = 64 * 1024;

        ncl::HttpServer server(8080, "127.0.0.1", options);
        server.get("/hello", [](const ncl::HttpServerRequest& request) {
            const auto name = request.queryParam("name").value_or("world");
            if (name.empty() || name.size() > 80) {
                return ncl::HttpResponse::text("Name must be 1..80 bytes", 400);
            }
            return ncl::HttpResponse::text("Hello, " + name + "!");
        });

        std::cout << "Listening: http://127.0.0.1:8080/hello" << std::endl;
        server.serve();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

What happens:

1. `HttpOptions` sets a five-second budget for each accepted request and a 64 KiB body cap.
2. The constructor binds port 8080 on IPv4 loopback. Only this computer can connect.
3. `get` registers a path and a handler, the function that returns its response.
4. `queryParam` reads `name` from the URL. Missing values use `world`.
5. `HttpResponse::text` constructs a 200 response, or a 400 response for invalid input.
6. `serve` waits for connections and calls handlers. The program remains running.

The timeout starts after a connection is accepted; it is not a five-second
lifetime for the server. The response is plain text, so the example does not
need JSON serialization or HTML escaping.

## Step 3: write the client

**File: `hello_client.cpp`**

```cpp
#include "NCL.hpp"
#include <chrono>
#include <exception>
#include <iostream>

int main() {
    try {
        ncl::HttpOptions options;
        options.wait.timeout = std::chrono::seconds(5);
        options.maxBodyBytes = 64 * 1024;

        ncl::HttpClient client;
        const auto response = client.get(
            "http://127.0.0.1:8080/hello?name=NCL", options);
        std::cout << response.statusCode << '\n' << response.body << '\n';
        return response.statusCode == 200 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

An HTTP error status such as 404 is a valid `HttpResponse`, not an exception.
Check `statusCode`. Transport errors and invalid protocol data throw exceptions;
see [error kinds](API.md#errors).

## Step 4: compile

Windows with GCC/MinGW:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic hello_server.cpp -o hello_server.exe -lws2_32
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic hello_client.cpp -o hello_client.exe -lws2_32
```

Windows with MSVC, in a Visual Studio developer prompt:

```bat
cl /EHsc /std:c++17 /O2 /W4 /utf-8 hello_server.cpp /Fe:hello_server.exe
cl /EHsc /std:c++17 /O2 /W4 /utf-8 hello_client.cpp /Fe:hello_client.exe
```

Linux or WSL:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic hello_server.cpp -o hello_server
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic hello_client.cpp -o hello_client
```

Compile each source as a separate program; they each contain `main`.
More build options are in [Build and integration](BUILDING.md).

## Step 5: run and check the result

In terminal 1 on Windows:

```powershell
.\hello_server.exe
```

Keep it open. In terminal 2, from the same folder:

```powershell
.\hello_client.exe
```

On Linux use `./hello_server` and `./hello_client` in separate terminals.
The client should print:

```text
200
Hello, NCL!
```

You can also open [the hello endpoint](http://127.0.0.1:8080/hello?name=NCL)
in a browser, or run:

```sh
curl --include "http://127.0.0.1:8080/hello?name=NCL"
```

On Windows use `curl.exe` if `curl` is a PowerShell alias. An unknown path
returns 404; `?name=` returns 400. Stop this introductory server with Ctrl+C.
For cleanup controlled by your program, follow the
[cancellation example](SERVER.md#step-8-stop-the-server-cleanly).

## Step 6: choose the next lesson

| Goal | Continue with |
| --- | --- |
| Build a server with several routes and HTTPS | [Server tutorial](SERVER.md). |
| Make POST, PUT, PATCH or DELETE requests | [HTTP requests](HTTP.md#create-and-inspect-client-requests). |
| Store and update simple in-memory resources | [Notes CRUD API](HTTP.md#build-a-small-create-read-update-delete-api). |
| Use a custom connected protocol | [TCP server/client](TCP.md#first-server-and-client). |
| Exchange independent packets | [UDP server/client](UDP.md#first-udp-server-and-client). |
| Find a signature, default or error | [API reference](API.md). |
