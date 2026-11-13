# LibChdKit for .NET

MAME 0.289's CHD tools (the code behind `chdman`) as a .NET library. It
creates, verifies, copies and extracts CHD images for CD (cue/bin, gdi, toc,
nrg, iso), DVD, hard disk, raw and laserdisc media. The CHD bytes it writes
are identical to upstream chdman's.

Runs on Windows x64, Windows ARM64 and macOS (Apple silicon and Intel) with
.NET 8 or newer. The package carries the native `chdkit.dll` /
`libchdkit.dylib`, so nothing else needs installing.

## Install

```
dotnet add package LibChdKit
```

## Use

```csharp
using LibChdKit;

using var chdman = new Chdman();
chdman.Progress = new Progress<ChdmanProgress>(p => progressBar.Value = p.Fraction * 100);
chdman.Log = (level, message) => Debug.WriteLine($"{level}: {message}");

// operations block, so run them off the UI thread
await Task.Run(() => chdman.CreateCd(@"C:\games\game.cue", @"C:\games\game.chd", cancellationToken: token));

var info = chdman.Info(@"C:\games\game.chd");
Console.WriteLine($"{info.MediaType} {info.Compression} {info.RawSha1}");

var result = chdman.Verify(@"C:\games\game.chd");
if (result.Outcome != ChdVerifyOutcome.Ok) { /* ... */ }

chdman.ExtractCd(@"C:\games\game.chd", @"C:\out\game.cue");
```

Everything chdman does is there: `Info`, `Verify`, `CreateRaw`,
`CreateHardDisk`, `CreateCd`, `CreateDvd`, `CreateLaserdisc`, `Copy`,
`ExtractRaw` (also hard disk and DVD), `ExtractCd`, `ExtractLaserdisc`,
`AddMetadata`, `AddMetadataFromFile`, `DeleteMetadata`, `GetMetadata`,
`DumpMetadata` and `Chdman.Templates`. Compression, hunk size, parent CHDs,
overwrite and input ranges are set with `ChdOutputSettings` and `ChdRange`;
defaults match chdman.

`ExtractCdToSink` and `ExtractRawToSink` stream instead of writing files:
they hand each file `ExtractCd` / `ExtractRaw` would write (for a CD, the
TOC first, then the bins in track order) to a `ChdExtractSink` as
`BeginFile(name)`, `Write(ReadOnlySpan<byte>)` blocks and `EndFile()`. The
bytes are identical to the files', and nothing touches the disk, which
suits hashing tracks or converting sectors on the fly:

```csharp
sealed class HashingSink : ChdExtractSink
{
    IncrementalHash? _sha1;
    public override void BeginFile(string name) => _sha1 = IncrementalHash.CreateHash(HashAlgorithmName.SHA1);
    public override void Write(ReadOnlySpan<byte> data) => _sha1!.AppendData(data);
    public override void EndFile() => Console.WriteLine(Convert.ToHexString(_sha1!.GetHashAndReset()));
}

chdman.ExtractCdToSink(@"C:\games\game.chd", "game.cue", new HashingSink(), outputBin: "game (Track %t).bin", splitBin: true);
```

Rules:

- A `Chdman` instance runs one operation at a time (a second concurrent call
  throws `InvalidOperationException`). Use one instance per worker for
  parallel jobs.
- Failures throw `ChdmanException` with a `Status` and chdman's message,
  which names the cause (for example the `.bin` a `.cue` points to but
  that doesn't exist).
- A cancelled operation (through its `CancellationToken`, or `Cancel()` from
  any thread) deletes its partial output and throws
  `OperationCanceledException` (`ChdmanException` with status `Cancelled`
  when `Cancel()` was used). `Cancel()` while nothing is running cancels the
  next operation on that instance.
- `Progress` and `Log` are called on the operation's thread. With
  `Progress<T>`, reports are posted to the UI thread for you. If a callback
  throws, the operation is cancelled and that exception is rethrown.
- Metadata can only be added or removed in uncompressed CHDs (as in chdman).
- Dispose the instance when done; if you forget, the finalizer frees it.

## Building the package

The package needs the native library for each platform it supports. Build
lib-chd-kit (see the top-level README) and put the results here:

```
artifacts/native/win-x64/chdkit.dll        (Windows x64 Release build)
artifacts/native/win-arm64/chdkit.dll      (Windows ARM64 Release build)
artifacts/native/osx/libchdkit.dylib       (macOS universal build)
```

then, from `bindings/csharp/LibChdKit`:

```
dotnet pack -c Release -o ../../../artifacts/nuget
```

A platform whose library is missing is left out with a build warning, so a
Windows-only package can be built on Windows alone. Pass
`-p:ChdmanNativeRoot=<dir>` to use a different folder.

## Tests

`LibChdKit.Tests` is a self-checking console program (no test framework).
Point it at a native build and run it:

```
dotnet run -c Release -p:ChdmanLibDir=../../../build-x64/Release
```
