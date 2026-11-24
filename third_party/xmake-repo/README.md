# Local xmake package overrides

This directory is an xmake package repository used only by the Windows build
(`--backend=win32`). `xmake.lua` adds it with:

```lua
add_repositories("puck-kiosk-packages third_party/xmake-repo")
```

## libdatachannel and IXWebSocket

`packages/l/libdatachannel/xmake.lua` is a copy of the xmake-repo port for
libdatachannel 0.24.6 (xmake-repo commit `6856000af`). Two changes are needed
for the mingw cross build:

- The upstream port declares `on_install("!mingw", ...)`, so xmake refuses to
  cross-build it even though libdatachannel's CMake project supports the
  toolchain. The copy removes that exclusion.
- usrsctp's Windows header defines `uint16_t`/`uint32_t` as macros unless it
  includes `<stdint.h>`. libdatachannel uses C++ functional casts such as
  `uint16_t(x)`, so the copy injects
  `add_compile_definitions(SCTP_STDINT_INCLUDE=<stdint.h>)` into its
  CMakeLists. It still depends on OpenSSL 3, which the xmake-repo `openssl3`
  package builds with mingw.

`packages/i/ixwebsocket/xmake.lua` adds an `openssl3` option to the xmake-repo
port. Upstream only offers `openssl` (1.1.1) or `mbedtls`; using OpenSSL 3 for
both IXWebSocket and libdatachannel avoids linking two copies of OpenSSL into
one executable.

Native Windows builds use this repository too: the package overrides are
platform-guarded, so the same pinned versions build with MSVC. vcpkg is only an
alternative if the operator switches the win32 `add_requires` block to system
packages (see the Windows section of the README).
