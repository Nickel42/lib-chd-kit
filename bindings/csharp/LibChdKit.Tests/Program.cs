// Checks for the .NET binding. Exit code 0 = all checks passed.
// SPDX-License-Identifier: BSD-3-Clause

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;
using LibChdKit;
using LibChdKit.Interop;

int checks = 0, failures = 0;
void Check(bool ok, string what)
{
    checks++;
    if (!ok)
    {
        failures++;
        Console.Error.WriteLine($"  FAILED: {what}");
    }
}
void Throws<T>(Action action, string what, Func<T, bool>? also = null) where T : Exception
{
    try
    {
        action();
        Check(false, $"{what} (no exception)");
    }
    catch (T ex)
    {
        Check(also == null || also(ex), $"{what} ({ex.GetType().Name}: {ex.Message})");
    }
    catch (Exception ex)
    {
        Check(false, $"{what} (wrong exception {ex.GetType().Name}: {ex.Message})");
    }
}

var dir = Path.Combine(Path.GetTempPath(), "lib-chd-kit-dotnet-tests");
if (Directory.Exists(dir))
    Directory.Delete(dir, recursive: true);
Directory.CreateDirectory(dir);
string P(string name) => Path.Combine(dir, name);

Console.WriteLine($"lib-chd-kit {Chdman.Version}, ABI {Chdman.AbiVersion}, .NET {Environment.Version}");

// struct layouts must match the C sizeof values (x64 / arm64)
Check(Marshal.SizeOf<CreateRawOptions>() == 88, "sizeof create_raw_options");
Check(Marshal.SizeOf<CreateHdOptions>() == 120, "sizeof create_hd_options");
Check(Marshal.SizeOf<VerifyResult>() == 176, "sizeof verify_result");
Check(Marshal.SizeOf<MetadataEntry>() == 40, "sizeof metadata_entry");

// templates
var templates = Chdman.Templates;
Check(templates.Count == 17, "17 templates");
Check(templates[0].Manufacturer == "Conner" && templates[0].Cylinders == 332, "template 0 is the Conner CFA170A");

// test image: noise, ramp, zeros, and a repeat of the noise (dedupes)
var image = new byte[4 * 65536 + 512 * 100];
var rng = new Random(1234);
rng.NextBytes(image.AsSpan(0, 65536));
for (int i = 65536; i < 2 * 65536; i++) image[i] = (byte)(i / 7);
Array.Copy(image, 0, image, 3 * 65536, 65536);
File.WriteAllBytes(P("raw.img"), image);
string sha1 = Convert.ToHexString(SHA1.HashData(image)).ToLowerInvariant();

using (var chdman = new Chdman())
{
    var reports = new List<ChdmanProgress>();
    var logs = new List<(ChdmanLogLevel, string)>();
    chdman.Progress = new SyncProgress(reports.Add);
    chdman.Log = (level, msg) => logs.Add((level, msg));

    // create / info / verify / extract
    chdman.CreateRaw(P("raw.img"), P("raw.chd"), 512);
    Check(reports.Any(r => r.Phase == ChdmanPhase.Compressing && r.Fraction == 1.0), "final compressing progress report");
    Check(logs.Any(l => l.Item2.Contains("Compression complete")), "log line 'Compression complete'");

    var info = chdman.Info(P("raw.chd"), hunkStats: true);
    Check(info.Version == 5, "CHD version 5");
    Check(info.LogicalBytes == (ulong)image.Length, "logical size");
    Check(info.HunkBytes == 4096, "default hunk size");
    Check(info.Codecs.SequenceEqual(new[] { "lzma", "zlib", "huff", "flac" }), $"default codecs ({string.Join(",", info.Codecs)})");
    Check(info.IsCompressed, "compressed");
    Check(info.MediaType == "raw", "media type raw");
    Check(info.RawSha1 == sha1, "raw SHA-1 matches the input");
    Check(info.HunkStats.Sum(s => s.Hunks) == info.HunkCount, "hunk stats add up");
    Check(info.HunkStats.Any(s => s.Name == "Copy from self"), "dedupe hunk stat");

    var verify = chdman.Verify(P("raw.chd"));
    Check(verify.Outcome == ChdVerifyOutcome.Ok, "verify OK");
    Check(verify.ComputedRawSha1 == sha1, "verify computed SHA-1");

    chdman.ExtractRaw(P("raw.chd"), P("raw.out"));
    Check(File.ReadAllBytes(P("raw.out")).SequenceEqual(image), "raw round trip");
    chdman.ExtractRaw(P("raw.chd"), P("part.out"), force: true, range: new ChdRange { StartHunk = 1, LengthBytes = 1000 });
    Check(File.ReadAllBytes(P("part.out")).SequenceEqual(image.Skip(4096).Take(1000)), "partial extraction");

    // copy with new codecs and hunk size
    chdman.Copy(P("raw.chd"), P("copy.chd"), new ChdOutputSettings { Compression = "zstd", HunkSize = 8192 });
    var copy = chdman.Info(P("copy.chd"));
    Check(copy.Codecs.SequenceEqual(new[] { "zstd" }) && copy.HunkBytes == 8192 && copy.RawSha1 == sha1, "copy recompresses");

    // errors
    Throws<ChdmanException>(() => chdman.CreateRaw(P("raw.img"), P("raw.chd"), 512), "existing output", e => e.Status == ChdmanStatus.FileExists);
    Throws<ChdmanException>(() => chdman.CreateRaw(P("raw.img"), P("bad.chd"), 512, new ChdOutputSettings { Compression = "abcd" }), "bad codec", e => e.Status == ChdmanStatus.InvalidArgument && e.Message.Contains("abcd"));
    Throws<ChdmanException>(() => chdman.Info(P("raw.img")), "info on a non-CHD");

    // a .cue naming a missing .bin reports the cause in the message and the log
    File.WriteAllText(P("missing.cue"), "FILE \"nothere.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n");
    logs.Clear();
    Throws<ChdmanException>(() => chdman.CreateCd(P("missing.cue"), P("missing.chd")), "cue with missing bin", e => e.Message.Contains("nothere.bin"));
    Check(logs.Any(l => l.Item1 == ChdmanLogLevel.Error && l.Item2.Contains("nothere.bin")), "error-level log line names the missing bin");

    // metadata on an uncompressed CHD
    chdman.CreateRaw(P("raw.img"), P("plain.chd"), 512, new ChdOutputSettings { Compression = "none" });
    chdman.AddMetadata(P("plain.chd"), "TEST", "hello wörld");
    chdman.AddMetadata(P("plain.chd"), "BIN", new byte[] { 1, 2, 3 }, noChecksum: true);
    Check(chdman.GetMetadata(P("plain.chd"), "BIN").SequenceEqual(new byte[] { 1, 2, 3 }), "binary metadata round trip");
    var plain = chdman.Info(P("plain.chd"));
    Check(plain.Metadata.Count == 2, "two metadata items");
    Check(plain.Metadata[0].TagString == "TEST" && plain.Metadata[0].Text == "hello wörld" && plain.Metadata[0].Checksummed, "text metadata (UTF-8)");
    Check(plain.Metadata[1].TagString == "BIN " && !plain.Metadata[1].Checksummed, "binary metadata flags");
    chdman.DumpMetadata(P("plain.chd"), "TEST", P("meta.txt"));
    Check(new FileInfo(P("meta.txt")).Length == System.Text.Encoding.UTF8.GetByteCount("hello wörld") + 1, "dumped metadata");
    chdman.DeleteMetadata(P("plain.chd"), "TEST");
    Throws<ChdmanException>(() => chdman.GetMetadata(P("plain.chd"), "TEST"), "deleted metadata", e => e.Status == ChdmanStatus.NotFound);

    // blank hard disk from a template
    chdman.CreateHardDisk(P("blank.chd"), new ChdHardDiskOptions { TemplateId = 1 });
    var blank = chdman.Info(P("blank.chd"));
    Check(blank.MediaType == "hd" && blank.LogicalBytes == 321UL * 2 * 16 * 512 && !blank.IsCompressed, "blank hard disk");

    // cancellation through a CancellationToken removes the partial output
    using (var cts = new CancellationTokenSource())
    {
        chdman.Progress = new SyncProgress(_ => cts.Cancel());
        Throws<OperationCanceledException>(() => chdman.CreateRaw(P("raw.img"), P("cancel.chd"), 512, cancellationToken: cts.Token), "token cancels");
        Check(!File.Exists(P("cancel.chd")), "cancelled output removed");
        Throws<OperationCanceledException>(() => chdman.Info(P("raw.chd"), cancellationToken: cts.Token), "already-cancelled token");
    }

    // an exception thrown by a callback cancels the operation and surfaces unchanged
    chdman.Progress = new SyncProgress(_ => throw new InvalidDataException("from callback"));
    Throws<InvalidDataException>(() => chdman.CreateRaw(P("raw.img"), P("throw.chd"), 512), "callback exception propagates");
    Check(!File.Exists(P("throw.chd")), "output removed after callback exception");
    chdman.Progress = null;

    // the instance works again afterwards
    chdman.CreateRaw(P("raw.img"), P("after.chd"), 512);
    Check(chdman.Info(P("after.chd")).RawSha1 == sha1, "usable after cancel and callback failure");

    // one operation at a time per instance
    using (var gate = new ManualResetEventSlim())
    using (var release = new ManualResetEventSlim())
    {
        chdman.Progress = new SyncProgress(_ => { gate.Set(); release.Wait(); });
        var running = Task.Run(() => chdman.CreateRaw(P("raw.img"), P("busy.chd"), 512, new ChdOutputSettings { Force = true }));
        gate.Wait();
        Throws<InvalidOperationException>(() => chdman.Info(P("raw.chd")), "concurrent use rejected");
        release.Set();
        running.Wait();
        chdman.Progress = null;
    }

    // Cancel() from another thread while an operation runs
    using (var gate = new ManualResetEventSlim())
    {
        chdman.Progress = new SyncProgress(_ => { gate.Set(); Thread.Sleep(20); });
        var running = Task.Run(() => chdman.CreateRaw(P("raw.img"), P("cancel2.chd"), 512));
        gate.Wait();
        chdman.Cancel();
        try { running.Wait(); Check(false, "Cancel() from another thread"); }
        catch (AggregateException ex) { Check(ex.InnerException is ChdmanException { Status: ChdmanStatus.Cancelled }, $"Cancel() from another thread ({ex.InnerException?.Message})"); }
        chdman.Progress = null;
    }

    // streaming extraction: the same bytes as the files ExtractCd/ExtractRaw write
    var disc = new byte[(300 + 151) * 2352];
    rng.NextBytes(disc);
    for (int f = 0; f < 300; f++)
    {
        var s = disc.AsSpan(f * 2352, 16);
        s.Clear();
        s.Slice(1, 10).Fill(0xff);
        s[15] = 1; // mode 1 header
    }
    File.WriteAllBytes(P("disc.bin"), disc);
    File.WriteAllText(P("disc.cue"), "FILE \"disc.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:04:00\n");
    chdman.CreateCd(P("disc.cue"), P("disc.chd"));

    var virtualDir = Path.Combine(dir, "virtual");
    foreach (var (toc, bin, split, count) in new[] { ("c.cue", (string?)null, false, 2), ("s.cue", "s %t.bin", true, 3), ("g.gdi", null, false, 3), ("t.toc", null, false, 2) })
    {
        chdman.ExtractCd(P("disc.chd"), P(toc), bin is null ? null : P(bin), split);
        var sink = new CollectingSink();
        chdman.ExtractCdToSink(P("disc.chd"), Path.Combine(virtualDir, toc), sink, bin is null ? null : Path.Combine(virtualDir, bin), split);
        Check(sink.Files.Count == count, $"{toc}: {count} files streamed ({sink.Files.Count})");
        Check(sink.Files.Count > 0 && sink.Files[0].Name == Path.Combine(virtualDir, toc), $"{toc}: TOC first");
        Check(sink.Files.All(f => Path.GetDirectoryName(f.Name) == virtualDir && File.ReadAllBytes(P(Path.GetFileName(f.Name))).SequenceEqual(f.Data.ToArray())),
            $"{toc}: streamed files match the extracted ones");
    }
    Check(!Directory.Exists(virtualDir), "streaming writes nothing");
    var cueSink = new CollectingSink();
    chdman.ExtractCdToSink(P("disc.chd"), "x.cue", cueSink);
    Check(cueSink.Files.Count == 2 && cueSink.Files[1].Name == "x.bin" && cueSink.Files[1].Data.ToArray().SequenceEqual(disc), "streamed bin equals the original bin");

    var rawSink = new CollectingSink();
    chdman.ExtractRawToSink(P("raw.chd"), "whole.raw", rawSink);
    Check(rawSink.Files.Count == 1 && rawSink.Files[0].Name == "whole.raw" && rawSink.Files[0].Data.ToArray().SequenceEqual(image), "raw to sink");
    var partSink = new CollectingSink();
    chdman.ExtractRawToSink(P("raw.chd"), "part.raw", partSink, new ChdRange { StartHunk = 1, LengthBytes = 1000 });
    Check(partSink.Files.Count == 1 && partSink.Files[0].Data.ToArray().SequenceEqual(image.Skip(4096).Take(1000)), "partial raw to sink");

    // a sink that throws cancels the operation; a cancelled token stops it
    Throws<InvalidDataException>(() => chdman.ExtractCdToSink(P("disc.chd"), "x.cue", new CollectingSink { ThrowOnData = true }), "sink exception propagates");
    using (var cts = new CancellationTokenSource())
    {
        var sink = new CollectingSink { OnData = cts.Cancel };
        Throws<OperationCanceledException>(() => chdman.ExtractRawToSink(P("raw.chd"), "x.raw", sink, cancellationToken: cts.Token), "token cancels a streaming extraction");
    }
    var again = new CollectingSink();
    chdman.ExtractCdToSink(P("disc.chd"), "x.gdi", again);
    Check(again.Files.Count == 3, "usable after a failed streaming extraction");
}

// a disposed instance refuses work, and Cancel() after Dispose is harmless
var disposed = new Chdman();
disposed.Dispose();
disposed.Cancel();
Throws<ObjectDisposedException>(() => disposed.Info(P("raw.chd")), "use after Dispose");

// separate instances run in parallel
var results = Enumerable.Range(0, 3).AsParallel().Select(i =>
{
    using var c = new Chdman();
    c.CreateRaw(P("raw.img"), P($"par{i}.chd"), 512);
    return c.Info(P($"par{i}.chd")).RawSha1;
}).ToList();
Check(results.All(r => r == sha1), "parallel instances");

Console.WriteLine($"{checks} checks, {failures} failures");
return failures == 0 ? 0 : 1;

// Collects streamed files in memory and checks the call order.
sealed class CollectingSink : ChdExtractSink
{
    public List<(string Name, MemoryStream Data)> Files { get; } = new();
    public bool ThrowOnData { get; set; }
    public Action? OnData { get; set; }
    bool _open;

    public override void BeginFile(string name)
    {
        if (_open) throw new InvalidOperationException("BeginFile while a file is open");
        Files.Add((name, new MemoryStream()));
        _open = true;
    }

    public override void Write(ReadOnlySpan<byte> data)
    {
        if (!_open) throw new InvalidOperationException("Write outside a file");
        if (ThrowOnData) throw new InvalidDataException("from sink");
        OnData?.Invoke();
        Files[^1].Data.Write(data);
    }

    public override void EndFile()
    {
        if (!_open) throw new InvalidOperationException("EndFile outside a file");
        _open = false;
    }
}

// Progress<T> posts to the thread pool; tests want reports inline.
sealed class SyncProgress : IProgress<ChdmanProgress>
{
    readonly Action<ChdmanProgress> _report;
    public SyncProgress(Action<ChdmanProgress> report) => _report = report;
    public void Report(ChdmanProgress value) => _report(value);
}
