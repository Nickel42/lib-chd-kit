# Chdman for Swift (macOS)

MAME 0.289's CHD tools (the code behind `chdman`) as a Swift package. It
creates, verifies, copies and extracts CHD images for CD (cue/bin, gdi, toc,
nrg, iso), DVD, hard disk, raw and laserdisc media. The CHD bytes it writes
are identical to upstream chdman's.

macOS 11 or newer, Apple silicon and Intel. The native library comes as a
prebuilt static XCFramework from the GitHub release, so apps don't build any
C++.

## Install

In Xcode: File > Add Package Dependencies, enter
`https://github.com/Nickel42/lib-chd-kit`. Or in `Package.swift`:

```swift
.package(url: "https://github.com/Nickel42/lib-chd-kit", from: "1.0.0"),
// ...
.target(name: "MyApp", dependencies: [.product(name: "Chdman", package: "lib-chd-kit")]),
```

## Use

```swift
import Chdman

let chdman = try Chdman()
chdman.onProgress = { p in
    DispatchQueue.main.async { progressView.doubleValue = p.fraction }
    return false                       // true cancels
}

// run() moves the blocking work off the current actor; cancelling the
// task cancels the operation and deletes its partial output
try await chdman.run { try $0.createCD(input: "/path/game.cue", output: "/path/game.chd") }

let info = try chdman.info("/path/game.chd")
print(info.mediaType, info.compression, info.rawSHA1)

if try chdman.verify("/path/game.chd").outcome != .ok { /* ... */ }
try chdman.extractCD(input: "/path/game.chd", output: "/out/game.cue")
```

Everything chdman does is there: `info`, `verify`, `createRaw`,
`createHardDisk`, `createCD`, `createDVD`, `createLaserdisc`, `copy`,
`extractRaw` (also hard disk and DVD), `extractCD`, `extractLaserdisc`,
`addMetadata` (text, data or a file), `deleteMetadata`, `metadata`,
`dumpMetadata` and `Chdman.templates`. Compression, hunk size, parent CHDs,
overwrite and input ranges are set with `ChdOutputSettings` and `ChdRange`;
defaults match chdman.

Rules:

- A `Chdman` instance runs one operation at a time (a second concurrent call
  throws). Use one instance per worker for parallel jobs.
- Failures throw `ChdmanError` with a `status` and chdman's message, which
  names the cause (for example the `.bin` a `.cue` points to but that
  doesn't exist).
- `cancel()` works from any thread; while nothing is running it cancels the
  next operation on that instance. Inside `run`, task cancellation throws
  `CancellationError`.
- `onProgress` and `onLog` are called on the operation's thread, not the
  main thread.
- Metadata can only be added or removed in uncompressed CHDs (as in chdman).

## Releasing and local development

`scripts/make-xcframework.sh` builds `build-xcframework/CChdman.xcframework`
(universal static library, symbols other than the C API hidden). To work
against it instead of the release download:

```
scripts/make-xcframework.sh
LIBCHDKIT_LOCAL_XCFRAMEWORK=1 swift test
```

To publish a release, run `scripts/make-xcframework.sh --release 1.0.0`. It
zips the XCFramework and writes the version and checksum into
`Package.swift`. Commit that, tag `v1.0.0`, and attach
`build-xcframework/CChdman.xcframework.zip` to the `v1.0.0` GitHub release.
Until the first release exists, only the local mode works.
