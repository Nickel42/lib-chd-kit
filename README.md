# lib-chd-kit

MAME 0.289's CHD (Compressed Hunks of Data) tooling, the code behind
`chdman`, packaged as a library with a stable C ABI for Windows 11
(MSVC, x64 and ARM64), macOS (universal arm64 + x86_64) and Linux (x64,
GCC). It is built
from the upstream MAME sources at tag `mame0289`, not from a decompilation.

- `include/chdman.h`: C ABI with opaque handles, status codes plus a message
  getter, typed option structs, a progress callback with cancellation, and a
  log callback. No exceptions, `printf`, or `exit` cross the API.
- Covers `info`, `verify` (with fix), create raw/hd/cd/dvd/ld, extract
  raw (also hd/dvd)/cd/ld, `copy`, add/delete/dump metadata, and the list of
  hard disk templates.
- `chdman-cli`: a chdman-compatible command line tool written in C on top of
  the C API.
- Output is byte-identical to upstream chdman (see [Verification](#verification)).

## Layout

```
include/chdman.h            public C header (+ module.modulemap for Swift)
src/chdman_core.{h,cpp}     chdman's do_* logic as a C++ library core
src/chdman_capi.cpp         C ABI layer (catches everything, maps to status codes)
cli/chdman_cli.c            chdman-compatible CLI (pure C, uses only chdman.h)
tests/                      round-trip/determinism tests, upstream parity test
bindings/csharp/            .NET binding + NuGet package (LibChdKit)
bindings/swift/, Package.swift  Swift package (Chdman) over a prebuilt XCFramework
examples/swift, examples/csharp
scripts/make-xcframework.sh builds the XCFramework for the Swift package
vendor/mame/                vendored MAME 0.289 subset (see vendor/PATCHES.md)
vendor/osd_shim.cpp         minimal OSD layer replacing MAME's osdlib
THIRD_PARTY_NOTICES.md      licenses of everything bundled
```

## Building

Requires CMake 3.20+ and a C++20 compiler (MAME 0.289 uses C++20).

### macOS (universal dylib + static library)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

This produces `build/libchdkit.dylib` (install name `@rpath/libchdkit.1.dylib`),
`build/libchdkit.a` and `build/chdman-cli`, all universal. The deployment
target defaults to macOS 11.0 (`-DCMAKE_OSX_DEPLOYMENT_TARGET=...` to change).
Without Ninja, use `-G Xcode` or the default Makefiles.

The dylib exports only the `chdman_*` functions. The static archive is
prelinked into a single object per architecture in which every bundled
symbol (zlib, zstd, FLAC, LZMA, MAME) is private, so it can't clash with
copies your app links itself. Set `-DCHDMAN_STATIC_PRELINK=OFF` to get an
ordinary archive instead. Apps linking the static library must also link
`libc++` (`-lc++`, or the `CChdman` module map does it for Swift).

To package it for Xcode as an XCFramework:

```bash
mkdir -p build/xcf-headers && cp include/chdman.h include/module.modulemap build/xcf-headers/
```

```bash
xcodebuild -create-xcframework -library build/libchdkit.a -headers build/xcf-headers -output build/Chdman.xcframework
```

### Windows 11 (MSVC x64 and ARM64)

Use Visual Studio 2022 **17.5 or newer** (MAME's C++20 code and the
`/Zc:enumTypes` / `/Zc:templateScope` switches MAME's build uses need it),
with the "Desktop development with C++" workload and, for ARM64, the
"MSVC ARM64 build tools" component.

x64, from any prompt:

```bash
cmake -S . -B build-x64 -G "Visual Studio 17 2022" -A x64
```

```bash
cmake --build build-x64 --config Release
```

```bash
ctest --test-dir build-x64 -C Release --output-on-failure
```

ARM64 (cross-compiles from an x64 machine; run the tests on an ARM64 device):

```bash
cmake -S . -B build-arm64 -G "Visual Studio 17 2022" -A ARM64
```

```bash
cmake --build build-arm64 --config Release
```

Or with Ninja, from the matching "x64 Native Tools" / "ARM64 Native Tools"
developer prompt:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

Outputs: `chdkit.dll` + `chdkit.lib` (import library), `chdkit_static.lib`
and `chdman-cli.exe`.

Which one to link on Windows:

- **`chdkit.dll` (recommended).** It uses the static CRT, so it needs no
  VC++ redistributable, and it exports only the 60 `chdman_*` functions, so
  it can't clash with anything in your app.
- **`chdkit_static.lib`.** Define `CHDMAN_STATIC` before including
  `chdman.h` (the CMake target does this for you). It is built with the DLL
  CRT (`/MD`, Visual Studio's default for apps); set
  `CHDMAN_STATIC_MSVC_RUNTIME` to `MultiThreaded` if your app uses `/MT`.
  Unlike the macOS archive, it can't hide the bundled zlib, zstd, FLAC and
  LZMA symbols: if your app links its own copy of one of those, you'll get
  LNK2005 duplicate symbols, so use the DLL instead.

### Linux (x64, GCC)

Needs GCC 11 or newer (tested with GCC 11 on Ubuntu 22.04 and GCC 15 on
Ubuntu 26.04), CMake and make or Ninja (`build-essential cmake` on Ubuntu).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build --parallel
```

```bash
ctest --test-dir build --output-on-failure
```

Outputs: `build/libchdkit.so` (soname `libchdkit.so.1`), `build/libchdkit.a`
and `build/chdman-cli`. The shared library exports only the 60 `chdman_*`
functions (a linker version script, `cmake/chdman.map`, hides the rest) and
needs the glibc and libstdc++ of the system it was built on or newer, so build
on the oldest distribution you support. Link the static archive with
`-lstdc++ -lpthread` (the CMake target does this); like the Windows static
library, it doesn't hide the bundled zlib, zstd, FLAC and LZMA symbols.

### CMake options

| Option | Default | |
|---|---|---|
| `CHDMAN_BUILD_SHARED` | ON | dylib / DLL |
| `CHDMAN_BUILD_STATIC` | ON | static library |
| `CHDMAN_BUILD_CLI` | ON | `chdman-cli` |
| `CHDMAN_BUILD_TESTS` | ON | test suite (`ctest`) |
| `CHDMAN_STATIC_PRELINK` | ON | Apple: hide non-API symbols in the static library |
| `CHDMAN_BUILD_UPSTREAM_REFERENCE` | OFF | build unmodified upstream `chdman` and add the parity test |
| `CHDMAN_SHARED_MSVC_RUNTIME` | `MultiThreaded$<$<CONFIG:Debug>:Debug>` | MSVC: CRT for the DLL and CLI (static) |
| `CHDMAN_STATIC_MSVC_RUNTIME` | `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` | MSVC: CRT for the static library (DLL CRT) |

Passing `CMAKE_MSVC_RUNTIME_LIBRARY` sets the default for both runtimes. When
the two differ, the bundled code is compiled once for each.

`cmake --install build --prefix <dir>` installs the libraries, header, module
map, license files and a `chdmanConfig.cmake` (`find_package(chdman)`, then
link `chdman::chdman_shared` or `chdman::chdman_static`). The same names
work with `add_subdirectory()`.

## Using the C API

```c
#include "chdman.h"

static int CHDMAN_CALL on_progress(void *user, chdman_phase phase, double fraction, double ratio)
{
    printf("%.0f%%\r", fraction * 100.0);
    return 0;                      /* non-zero cancels */
}

chdman_context *ctx;
chdman_context_create(&ctx);
chdman_context_set_progress(ctx, on_progress, NULL);

chdman_create_cd_options o;
chdman_create_cd_options_init(&o);   /* always init: sets struct_size and defaults */
o.input = "game.cue";
o.out.output = "game.chd";
o.out.compression = NULL;            /* default: cdlz,cdzl,cdfl */

if (chdman_create_cd(ctx, &o) != CHDMAN_OK)
    fprintf(stderr, "failed: %s\n", chdman_last_error(ctx));

chdman_context_destroy(ctx);
```

Rules:

- One context per thread. Separate contexts can run in parallel.
  `chdman_context_cancel()` may be called from any thread; the running
  operation returns `CHDMAN_ERR_CANCELLED` and deletes its partial output.
- All strings are UTF-8, including Windows paths.
- Unset numeric options are `0` (sizes) or `CHDMAN_UNSET` (offsets and
  lengths); the `*_init` functions set these.
- Defaults match chdman 0.289 (and the reverse-engineered spec): raw, hd and dvd
  use `lzma,zlib,huff,flac`; cd uses `cdlz,cdzl,cdfl`; ld uses `avhu`. The default
  hunk size is the largest multiple of the unit size that is ≤ 4096 for raw/hd,
  4096 for dvd, 19584 (8 frames of 2448 bytes) for cd, and one frame for ld.
- `chdman_verify` returns `CHDMAN_OK` whenever verification ran. The verdict
  is in `result.outcome` (`OK`, `RAW_MISMATCH`, `OVERALL_MISMATCH`, `FIXED`,
  `NOT_APPLICABLE`).
- Informational lines chdman would print (e.g. "Compression: ...") go to
  the log callback, so a GUI can show or ignore them.

## Using from Swift (macOS)

Add the package `https://github.com/Nickel42/lib-chd-kit` and depend on the
`Chdman` product. It wraps the whole C API with Swift types, `throws`, and
an async `run` that ties cancellation to Swift tasks:

```swift
import Chdman

let chdman = try Chdman()
chdman.onProgress = { p in
    DispatchQueue.main.async { progressView.doubleValue = p.fraction }
    return false                      // true cancels
}
try await chdman.run { try $0.createCD(input: "/path/game.cue", output: "/path/game.chd") }
let info = try chdman.info("/path/game.chd")
print(info.mediaType, info.compression, info.rawSHA1)
```

The package downloads a prebuilt static XCFramework from the GitHub release.
`bindings/swift/README.md` covers the API rules, local development and how
to publish a release with `scripts/make-xcframework.sh`.

Without the package, `include/module.modulemap` defines the `CChdman`
module for using the C API directly: point `-I` at `include` and link
`chdkit`.

## Using from C# / .NET

`bindings/csharp/LibChdKit` is a .NET 8+ binding for the whole API, packed
as the `LibChdKit` NuGet package with the native libraries for win-x64,
win-arm64 and osx inside:

```csharp
using LibChdKit;

using var chdman = new Chdman();
chdman.Progress = new Progress<ChdmanProgress>(p => progressBar.Value = p.Fraction * 100);
await Task.Run(() => chdman.CreateCd(@"C:\games\game.cue", @"C:\games\game.chd", cancellationToken: token));
var info = chdman.Info(@"C:\games\game.chd");
Console.WriteLine($"{info.MediaType} {info.Compression} {info.RawSha1}");
```

Handles are `SafeHandle`s, every operation takes a `CancellationToken`, and
errors are `ChdmanException`s carrying chdman's message.
`bindings/csharp/README.md` covers the API rules, building the package and
the tests. The example in `examples/csharp` uses the binding:

```bash
cd examples/csharp && dotnet run -p:ChdmanLibDir=../../build-x64/Release
```

## CLI

`chdman-cli` accepts the same commands, long/short options and output
format as chdman 0.289:

```bash
./build/chdman-cli createcd -i game.cue -o game.chd
```

```bash
./build/chdman-cli info -i game.chd -v
```

```bash
./build/chdman-cli extractcd -i game.chd -o out.cue
```

Ctrl-C cancels cleanly (the partial output is removed).

## Behavior notes and differences from chdman

- The CHD bytes written are identical to upstream chdman's. Differences are
  confined to the tool layer:
  - Read errors on raw/hd/dvd input files abort compression with an error.
    Upstream ignores them (marked FIXME there) and could write garbage.
  - `addmeta --valuetext "..."` strips one pair of surrounding quotes
    (upstream intends this but writes the unstripped string). The library
    API takes text verbatim.
  - Write/flush errors at the end of extraction are reported.
  - Progress callbacks are throttled to 10 per second (chdman: 2).
  - The thread limit (`chdman_context_set_num_processors`) is per context
    rather than a process-wide global.
- Metadata can only be added or removed in **uncompressed** CHDs. MAME
  opens compressed v5 CHDs read-only, and upstream `chdman addmeta`/`delmeta`
  fail the same way. Use `copy -c none` first if needed.
- `extracthd` and `extractdvd` are the same operation as `extractraw`
  (`chdman_extract_raw`), as in upstream.

## Verification

Run on macOS 27 (Apple silicon), universal Release build:

- `chdman_tests` / `chdman_tests_static` (197 checks each, against the
  dylib and the static library):
  - Raw round trip: a generated 7 MB image with random, ramp, zero and
    repeated regions. Default codecs and hunk size are checked, and so is
    dedup ("Copy from self"). Extraction is byte-exact, including partial
    extraction by hunk and byte.
  - Header SHA-1: the data SHA-1 equals an independent SHA-1 of the input.
    Single-threaded and multi-threaded runs produce byte-identical CHD
    files. The raw and overall SHA-1s are pinned to golden values, so any
    platform or compiler that differs fails the test.
  - CD: a synthetic two-track cue/bin (MODE1/2352 plus a 151-frame AUDIO
    track, which forces track padding) goes through createcd, then
    extractcd to cue/bin (byte-identical .bin), then createcd again (same
    header SHA-1s). Split-bin extraction and copy are covered too, and the
    CD header SHA-1s are pinned.
  - DVD, HD (explicit CHS, blank from template), LD (an AVI generated with
    MAME's own aviio: createld, extractld, then createld again with equal
    SHA-1, plus a frame sub-range), copy with recompression and a new hunk
    size, metadata add/get/dump/delete, verify plus fix after corrupting the
    header SHA-1.
  - Error handling (existing output, bad codec, bad hunk size, conflicting
    ranges, ABI mismatch, NULL context), cancellation from the callback
    and via `chdman_context_cancel()` (partial output is removed), and the
    log callback.
- `parity_upstream` (with `-DCHDMAN_BUILD_UPSTREAM_REFERENCE=ON`): runs 14
  commands through unmodified upstream `chdman.cpp` and through
  `chdman-cli`. All 17 outputs are byte-identical: CHDs from every create
  mode, copies, cue/toc/gdi extraction, raw and AVI extraction.
- The dylib exports exactly the 60 `chdman_*` symbols on both slices. The
  prelinked static archive defines no other global symbols and links into a
  C program together with the system zlib.
- Windows: the whole tree (library, DLL, CLI, tests, upstream reference)
  cross-compiles and links with mingw-w64 GCC 16, and the DLL exports
  exactly the 60 API functions. **MSVC builds and the tests have not been run
  on Windows yet.** Run the commands above (and `ctest`) on a Windows 11
  machine before shipping. The pinned SHA-1s in the tests catch any
  platform difference.
- The x86_64 slice of the universal build was compiled but not executed:
  this Mac has no Rosetta 2.

## Licensing

lib-chd-kit's own code is BSD-3-Clause (`LICENSE`). Every bundled component
is BSD-3-Clause, MIT, Zlib or public domain; no GPL code is compiled in. The
per-file audit and full license texts are in `THIRD_PARTY_NOTICES.md`.
Applications that ship lib-chd-kit must reproduce those notices. Local changes
to the vendored MAME sources (two small ones) are listed in
`vendor/PATCHES.md`, which also describes how to move to a newer MAME release.
