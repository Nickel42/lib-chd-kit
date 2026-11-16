# Vendored source provenance and local patches

Source: https://github.com/mamedev/mame, tag `mame0289`
(commit `f34f02505e32c1993c6a782b6814232cbfc74e36`), fetched with a sparse
checkout. Only the files needed to build the CHD code path were copied; the
directory layout under `vendor/mame/` mirrors upstream.

Files under `vendor/mame/` are unmodified except for the following.

## `src/lib/util/chd.h`, `src/lib/util/chd.cpp`

1. Added `chd_file_compressor::compress_wait_idle()`, which blocks until the
   read and compression work queues are empty. lib-chd-kit's compressors call
   it from their destructors. If compression is abandoned part-way through
   (error or cancellation), this stops an I/O worker thread from calling
   `read_data()` on an object that is being destroyed. Upstream chdman
   never needed it because it exits the process on error.
2. Removed the two `fprintf(stderr, ...)` calls in
   `chd_file_compressor::async_read()`'s exception handlers. The error is
   still recorded in `m_read_error` and returned by `compress_continue()`.
   lib-chd-kit reports it through `chdman_last_error()`.
3. Sized `chd_file_compressor::m_codecs` as `WORK_MAX_THREADS + 1` instead
   of `WORK_MAX_THREADS`. A thread that calls `osd_work_queue_wait()` on a
   `WORK_QUEUE_FLAG_MULTI` queue helps run queued items with
   `threadid == queue->threads`, and `osd_work_queue_alloc()` caps
   `queue->threads` at `WORK_MAX_THREADS` (16). So on a host with 18 or
   more logical CPUs, `compress_wait_idle()` (change 1) made
   `async_compress_hunk()` read `m_codecs[16]`, one past the end, and
   crash. Upstream chdman never waits on the work queue, so it doesn't hit
   this.
4. Added `chd_file::hunk_source`, `set_hunk_source()` and `locate_hunk()`
   (all marked "lib-chd-kit"). `read_hunk()` first asks the hunk source, if
   one is set, for an already-decoded hunk. `locate_hunk()` returns where a
   v5 compressed CHD stores a hunk's bytes (codec slot, length, offset,
   CRC-16), following copies within the file. lib-chd-kit's
   `src/hunk_pipeline.cpp` uses them to read the stored bytes in file order
   on one thread and decode them on several, with the same CRC check as
   `read_hunk()`. Parent references and anything else `locate_hunk()`
   declines are still decoded by `read_hunk()` itself.

## `src/lib/util/hashing.cpp`

5. `sha1_creator::append()` hashes whole 64-byte blocks with the x86 SHA
   extensions (SHA-NI) when the CPU has them, checked once with `cpuid`
   (marked "lib-chd-kit"). Partial blocks, other CPUs and other
   architectures use the original scalar code. SHA-1 is the serial step of
   verify once hunks are decoded on several threads, and SHA-NI is about
   three times faster. The digests are the same; the pinned header SHA-1s
   in `tests/test_chdman.cpp` would catch a difference.

## `3rdparty/flac/src/libFLAC/include/config.h`

6. With MSVC on ARM64, `FLAC__HAS_NEONINTRIN` and `FLAC__HAS_A64NEONINTRIN`
   are 0 (marked "lib-chd-kit"): MSVC can't compile `lpc_intrin_neon.c`'s
   GCC-style vector initializers, so the Windows ARM64 build uses libFLAC's
   C code. GCC and Clang on ARM64 keep the NEON code. This is the only
   change that can make output differ: a Windows ARM64 build's `cdfl` hunks
   may be compressed differently from a NEON build's, as with any two
   libFLAC builds. Every such CHD still decodes to the same data.

None of the other changes affects the bytes written to CHD files or read
from them; `tests/parity.cmake`
checks that output is byte-identical to the unmodified upstream `chdman`
logic.

## Not vendored (replaced by `vendor/osd_shim.cpp`)

- `src/osd/modules/lib/osdlib_*.cpp`: only `osd_getenv()` was needed.
- `src/osd/modules/file/{posix,win}socket.cpp` and
  `{posix,win}ptty.cpp`: the socket, domain-socket and pty openers are stubs that return
  `std::errc::not_supported`, so special file names are treated as plain
  files.
- `src/lib/util/delegate.cpp` / `mfpresolve.cpp`: not used by the CHD path.

## Updating to a newer MAME

1. Sparse-checkout the new tag (see the paths in `vendor/CMakeLists.txt`).
2. Re-copy the files and re-apply the four `chd.*` changes, the
   `hashing.cpp` change and the libFLAC `config.h` change above.
3. Diff `src/tools/chdman.cpp` against the previous version and port any
   logic changes into `src/chdman_core.cpp`. That file keeps upstream's
   function names to make this easier.
4. Build with `-DCHDMAN_BUILD_UPSTREAM_REFERENCE=ON` and run `ctest`. The
   `parity_upstream` test compares output against the new upstream chdman.
5. Update the pinned header SHA-1s in `tests/test_chdman.cpp` only if the
   CHD format itself changed.
