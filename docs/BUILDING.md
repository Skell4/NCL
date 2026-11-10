# Build and integrate NCL

![Build & Integrate](../assets/docs/build.svg)

[Home](../README.md) | [Start here](GETTING_STARTED.md) | [Server tutorial](SERVER.md) | [API reference](API.md) | **Build**

## Requirements and folder layout

NCL requires C++17 and native Windows or Linux socket APIs. Copy `NCL.hpp`
into your application; no NCL binary, installation step or code generation
is required. Windows uses system Winsock. TLS is implemented inside the header.
macOS/BSD are not supported by this snapshot.

Save a guide's complete **File** block in your application folder beside
`NCL.hpp`. Compilation commands below run there. Replace `hello_server.cpp`
with the source you saved; build each program containing `main` separately.

## GCC or MinGW on Windows

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic hello_server.cpp -o hello_server.exe -lws2_32
.\hello_server.exe
```

The Winsock flag comes after the sources/objects. Use a Windows MinGW toolchain
for native executables. Standard compiler/C++ runtime DLL requirements still
depend on that toolchain; header-only does not mean a standalone OS-free binary.

## MSVC on Windows

Use a Visual Studio developer command prompt:

```bat
cl /EHsc /std:c++17 /O2 /W4 /utf-8 hello_server.cpp /Fe:hello_server.exe
hello_server.exe
```

The header supplies MSVC's Winsock link instruction. A normal terminal may not
have the compiler/include environment initialized. In PowerShell, prefix a
local executable with `.\` when running it.

## GCC on Linux or WSL

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic hello_server.cpp -o hello_server
./hello_server
```

Do not add `-lws2_32` on Linux. Add `-pthread` to compile and link programs that
create application threads, such as `managed_server.cpp`. NCL creates no
internal worker threads.

## Enable HTTPS

> [!IMPORTANT]
> Define `NCL_ENABLE_TLS` for **every translation unit that includes NCL.hpp**.
> A build-system definition is convenient because it also reaches indirect
> includes. Do not mix TLS-enabled and TLS-disabled copies of this header in
> one executable.

Windows GCC/MinGW:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -DNCL_ENABLE_TLS api_server.cpp -o api_server.exe -lws2_32
```

MSVC:

```bat
cl /EHsc /std:c++17 /O2 /W4 /utf-8 /DNCL_ENABLE_TLS api_server.cpp /Fe:api_server.exe
```

Linux:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -DNCL_ENABLE_TLS api_server.cpp -o api_server
```

You may instead put `#define NCL_ENABLE_TLS` before the first inclusion in
each relevant source, consistently. Defining it as `0` still enables TLS,
because the header checks whether the macro exists. Omit/undefine it to
disable TLS. `HttpClient::supportsHttps()` reports the compiled capability.

No OpenSSL headers, link flags, DLLs or executable are needed by an NCL
application. Windows loads OS randomness/trust services; Linux uses `getrandom`
and supported system CA bundle paths. Runtime certificate and key files remain
necessary for a TLS server. See [the certificate walkthrough](SERVER.md#step-6-enable-https).
Optimization is recommended for the software cryptography.

## CMake in your application

Save this as `CMakeLists.txt` beside `NCL.hpp` and `api_server.cpp`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyNclServer LANGUAGES CXX)

option(MY_SERVER_TLS "Enable NCL HTTPS" ON)
add_library(ncl INTERFACE)
target_include_directories(ncl INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ncl INTERFACE cxx_std_17)
if(WIN32)
    target_link_libraries(ncl INTERFACE ws2_32)
endif()
if(MY_SERVER_TLS)
    target_compile_definitions(ncl INTERFACE NCL_ENABLE_TLS)
endif()

add_executable(api_server api_server.cpp)
target_link_libraries(api_server PRIVATE ncl)
```

Configure/build with an installed CMake generator:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Single-configuration generators place the executable in `build/`; Visual Studio
typically uses `build/Release/`. Run it from your application folder, or pass
certificate paths relative to its actual working directory. For an application
using threads, add `find_package(Threads REQUIRED)` and link its executable to
`Threads::Threads`. Set `-DMY_SERVER_TLS=OFF` to omit TLS.

## Multiple source files

Include the same header normally in every source that uses NCL. There is no
`NCL_IMPLEMENTATION` macro or one special implementation file. Apply the TLS
definition consistently to all sources, preferably through a shared CMake
interface target as above. Recompile dependent files after changing it.

Do not include Winsock's older `winsock.h` before `NCL.hpp`. If your application
also needs `windows.h`, include `NCL.hpp` first or arrange Winsock2 inclusion
before Windows headers. Do not edit the `ncl::detail` implementation to configure
your application.

## Development checks in this folder

These commands run from the version root, which contains `test/` and `docs/`.
They are for contributors; application users do not need the test directory.

```powershell
python test/documentation_examples.py
python test/check_documentation.py --build-dir build-docs-plain
python test/check_documentation.py --build-dir build-docs-tls --tls
```

On Linux use `python3`; the last runner also accepts `--sanitize`. These checks
extract complete programs directly from the guides, compile them and exercise
the documented behavior. Examples stay in the documentation as their source.

For CMake-based regression tests, use your installed generator. For example,
on Windows with Visual Studio 2022:

```powershell
cmake -S test -B test/build-docs-msvc -G "Visual Studio 17 2022" -A x64 -DNCL_TEST_TLS=ON
cmake --build test/build-docs-msvc --config Release --parallel 4
ctest --test-dir test/build-docs-msvc -C Release --output-on-failure
python test/check_documentation.py --build-dir build-docs-msvc/Release --tls --skip-build
```

`NCL_TEST_TLS` configures the development project; applications use
`NCL_ENABLE_TLS`. Some development integration suites use Python SSL/OpenSSL
as independent peers and OpenSSL to create disposable certificates. This is
separate from NCL application dependencies. Historical results and full TLS
reproduction commands are in [TLS verification](../test/TLS_VERIFICA.md).
Current documentation results and compiler-specific observations are in
[the documentation review](../test/DOCS_REFRESH_VERIFICA.md), including strict
warning diagnostics encountered with MinGW and the TLS-disabled MSVC build.
