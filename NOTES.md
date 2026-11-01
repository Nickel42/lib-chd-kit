# Hand-over notes: continuing lib-chd-kit on Windows

These notes are for whoever picks up this project next, person or Claude
session. Read them with `README.md`, which has the full build and usage
docs.

## What this project is

lib-chd-kit packages MAME 0.289's CHD tooling (the logic of `chdman`) as a
cross-platform library with a stable C ABI (`include/chdman.h`). Targets:
Windows 11 (MSVC x64 and ARM64) and macOS (universal arm64 + x86_64). Apps
use it from C/C++, Swift (macOS) and C# via P/Invoke (Windows and macOS).

It is built from the upstream MAME source at tag `mame0289`
(commit `f34f02505e32c1993c6a782b6814232cbfc74e36`). A reverse-engineered
spec of `chdman.exe` (kept outside this repo, in `chdman_re/CHDMAN_SPEC.md`
on the original Mac) was used only to cross-check defaults. It agrees with
upstream on codecs and hunk sizes.

## Status

Done and verified on macOS 27.0.1 (Apple silicon):

- The universal `libchdkit.dylib`, `libchdkit.a` and `chdman-cli` build.
- `ctest` passes 5/5:
  - `chdman_tests` and `chdman_tests_static`: 197 checks each.
  - `parity_upstream`: unmodified upstream chdman and lib-chd-kit produce
    17 byte-identical outputs.
  - Two CLI smoke tests.
- The dylib exports only the 60 `chdman_*` functions. The static archive is
  prelinked so bundled zlib/zstd/FLAC symbols are private.
- `examples/swift` was compiled and run, against both the dylib and the
  static library.
- `examples/csharp` was built and run with .NET 10 on macOS. The marshaled
  struct sizes match C `sizeof`: create_raw=88, create_hd=120,
  verify_result=176, metadata_entry=40.
- Windows: the whole tree cross-compiles and links with mingw-w64 (GCC 16),
  and the DLL exports exactly the 60 API functions.

**Not done yet (the reason for moving to Windows):**

1. Build with MSVC (Visual Studio 2022 17.5+) for x64, and run `ctest`.
2. Build for ARM64 and run `ctest` on an ARM64 Windows device, if one is
   available.
3. Build with `-DCHDMAN_BUILD_UPSTREAM_REFERENCE=ON` so the parity test runs
   on Windows too.
4. Run `examples/csharp` against `chdkit.dll`.
5. (Optional) Run the macOS x86_64 slice on an Intel Mac or with Rosetta.
   It was compiled but never executed.

## Windows steps

Prerequisites: Visual Studio 2022 **17.5 or newer** with "Desktop
development with C++" (plus "MSVC ARM64 build tools" for ARM64), CMake
3.20+ (VS includes one), and optionally the .NET 8 SDK for the C# example.

From a "Developer PowerShell for VS 2022" in the repo root:

```
cmake -S . -B build-x64 -G "Visual Studio 17 2022" -A x64 -DCHDMAN_BUILD_UPSTREAM_REFERENCE=ON
cmake --build build-x64 --config Release
ctest --test-dir build-x64 -C Release --output-on-failure
```

ARM64 (cross-compiled from x64; run the tests on an ARM64 machine):

```
cmake -S . -B build-arm64 -G "Visual Studio 17 2022" -A ARM64
cmake --build build-arm64 --config Release
```

C# example:

```
cd examples/csharp
dotnet run -p:ChdmanLibDir=../../build-x64/Release
```

Expected results: every test passes, and the pinned SHA-1s in
`tests/test_chdman.cpp` match. The raw image hashes to
`54fd023f…3290` (raw) and `6207aff9…fe69` (overall); the CD image to
`70beb418…0ed7` and `40662ec1…b929`. A mismatch means a real platform
difference in the CHD output; investigate it, don't re-pin the values.

## Things most likely to need fixing on MSVC

The code has never been compiled with MSVC. If something breaks, look here
first:

- **Compiler flags.** `vendor/CMakeLists.txt` → `chdman_msvc_options()` mirrors
  MAME's own MSVC switches from `scripts/genie.lua`: `/bigobj /utf-8
  /Zc:preprocessor` plus, for C++, `/EHsc /Zc:__cplusplus /Zc:enumTypes
  /Zc:templateScope /Zc:u8EscapeEncoding`. MAME 0.289 requires C++20.
- **Warnings.** Vendored targets build with `/W0`; lib-chd-kit's own code with
  `/W3`.
- **Defines** come from `chdman_mame` in `vendor/CMakeLists.txt`:
  `OSD_WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
  _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS CRLF=3 LSB_FIRST`.
- **Windows-only sources:** `winfile.cpp`, `windir.cpp`, `winutil.cpp`,
  `winutf8.cpp`, and FLAC's `win_utf8_io.c`. `vendor/osd_shim.cpp` stubs the
  socket/pty openers (`win_open_socket`, `win_open_ptty`, ...).
- **Link dependencies:** `shlwapi` for `PathIsRelativeW`. If other Win32
  libraries come up missing, MAME links `user32 advapi32 shell32` too.
- **Output names:** the DLL is `chdkit.dll` (import lib `chdkit.lib`) and the
  static library is `chdkit_static.lib`. Consumers of the static library
  must define `CHDMAN_STATIC`; the CMake target does this.
- **CRT:** static by default (`/MT`), via `CMAKE_MSVC_RUNTIME_LIBRARY`.
- **Exports:** the `CHDMAN_API` macro (`__declspec(dllexport)` when
  `CHDMAN_BUILDING_SHARED` is defined) controls them. Check with
  `dumpbin /exports chdkit.dll`, which should list 60 `chdman_*` functions.
- **Test DLL copying:** the tests copy `chdkit.dll` next to the test exe
  after each build. The CLI is built in the same directory as the DLL.

## Design decisions worth knowing

- **Core:** `src/chdman_core.cpp` is upstream `chdman.cpp`'s `do_*` logic
  with the same function names, using typed option structs, a `session`
  for progress/cancel/log, and errors thrown as `chdman::error`.
  `src/chdman_capi.cpp` catches everything at the C boundary.
- **Edits to MAME code (three):** `compress_wait_idle()` added to
  `chd_file_compressor`, so cancelling mid-compression can't race the I/O
  thread; two `fprintf(stderr)` calls removed; and `m_codecs` given one
  extra slot for the thread that waits on the work queue (otherwise hosts
  with 18+ CPUs crash on cancel). See `vendor/PATCHES.md`.
  None of them changes the bytes written; the parity test proves it.
- **Metadata editing:** add and delete only work on *uncompressed* CHDs.
  This is upstream behavior (MAME opens compressed v5 CHDs read-only), not
  a bug.
- **Differences from upstream chdman (tool layer only):** read errors on
  input files now abort, the CLI strips quotes from `addmeta` text, the
  thread limit is per context, and progress is throttled to 10 updates per
  second.
- **Licensing:** everything compiled in is BSD-3-Clause, MIT, Zlib or public
  domain. The per-file audit is in `THIRD_PARTY_NOTICES.md`. Keep it that
  way: don't vendor FLAC's tools or MAME files with GPL headers.

## Updating to a newer MAME

`vendor/PATCHES.md` has the procedure: sparse checkout, re-copy, re-apply
the three `chd.*` patches, port any `chdman.cpp` changes into
`src/chdman_core.cpp`, then run the parity test.
