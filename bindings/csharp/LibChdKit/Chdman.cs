// Managed API for lib-chd-kit: MAME's CHD tools (chdman) as a .NET library.
// SPDX-License-Identifier: BSD-3-Clause

using System;
using System.Collections.Generic;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using LibChdKit.Interop;

namespace LibChdKit
{
    public enum ChdmanStatus : int
    {
        Ok = 0, InvalidArgument = 1, FileExists = 2, FileOpen = 3, IO = 4, Chd = 5,
        Unsupported = 6, NotFound = 7, Cancelled = 8, OutOfMemory = 9, Internal = 10, AbiMismatch = 11
    }

    public enum ChdmanPhase : int { WalkingParent = 0, Compressing = 1, Verifying = 2, Extracting = 3, Analyzing = 4 }

    /// <summary>Log levels. Treat values you don't know as <see cref="Warning"/>.</summary>
    public enum ChdmanLogLevel : int { Info = 0, Warning = 1, Error = 2 }

    public enum ChdVerifyOutcome : int { Ok = 0, RawMismatch = 1, OverallMismatch = 2, Fixed = 3, NotApplicable = 4 }

    /// <summary>A lib-chd-kit call failed. <see cref="Status"/> is the C status code.</summary>
    public sealed class ChdmanException : Exception
    {
        public ChdmanStatus Status { get; }
        public ChdmanException(ChdmanStatus status, string message) : base(message) { Status = status; }
    }

    /// <summary>One progress report. <see cref="Ratio"/> is compressed/uncompressed while compressing, otherwise 0.</summary>
    public readonly record struct ChdmanProgress(ChdmanPhase Phase, double Fraction, double Ratio);

    /// <summary>Settings shared by every create operation and <see cref="Chdman.Copy"/>.</summary>
    public sealed class ChdOutputSettings
    {
        /// <summary>null = default for the media type, "none", or up to four comma-separated codecs ("lzma,zlib,huff,flac").</summary>
        public string? Compression { get; set; }
        /// <summary>0 = default for the media type.</summary>
        public uint HunkSize { get; set; }
        /// <summary>Optional parent CHD to deduplicate against.</summary>
        public string? Parent { get; set; }
        /// <summary>Overwrite an existing output file.</summary>
        public bool Force { get; set; }
    }

    /// <summary>Input range. Set at most one start and at most one length.</summary>
    public sealed class ChdRange
    {
        public ulong? StartByte { get; set; }
        public ulong? StartHunk { get; set; }
        public ulong? LengthBytes { get; set; }
        public ulong? LengthHunks { get; set; }
    }

    /// <summary>Hard disk parameters for <see cref="Chdman.CreateHardDisk"/>.</summary>
    public sealed class ChdHardDiskOptions
    {
        /// <summary>Raw disk image. null creates a blank (zeroed, uncompressed) disk.</summary>
        public string? Input { get; set; }
        /// <summary>Blank disk size in bytes (only without <see cref="Input"/>).</summary>
        public ulong? Size { get; set; }
        /// <summary>Index into <see cref="Chdman.Templates"/>.</summary>
        public int? TemplateId { get; set; }
        /// <summary>Optional IDENTIFY data file; CHS is read from it.</summary>
        public string? IdentFile { get; set; }
        /// <summary>Set all three, or none to have them guessed.</summary>
        public uint Cylinders { get; set; }
        public uint Heads { get; set; }
        public uint Sectors { get; set; }
        /// <summary>0 = 512 (or the template's or parent's).</summary>
        public uint SectorSize { get; set; }
        public ChdRange? Range { get; set; }
    }

    public sealed record ChdMetadata(uint Tag, string TagString, uint Index, bool Checksummed, byte[] Data)
    {
        /// <summary>The data as UTF-8 text, without the trailing NUL chdman stores.</summary>
        public string Text => Encoding.UTF8.GetString(Data, 0, Data.Length > 0 && Data[^1] == 0 ? Data.Length - 1 : Data.Length);
    }

    public sealed record ChdHunkStat(string Name, uint Hunks);

    public sealed record ChdInfo(
        uint Version,
        ulong LogicalBytes,
        uint HunkBytes,
        uint HunkCount,
        uint UnitBytes,
        ulong UnitCount,
        ulong FileBytes,
        bool IsCompressed,
        IReadOnlyList<string> Codecs,
        string Compression,
        string Sha1,
        string RawSha1,
        string ParentSha1,
        string MediaType,
        IReadOnlyList<ChdMetadata> Metadata,
        IReadOnlyList<ChdHunkStat> HunkStats);

    public sealed record ChdVerifyResult(ChdVerifyOutcome Outcome, string HeaderRawSha1, string ComputedRawSha1, string HeaderSha1, string ComputedSha1);

    public sealed record ChdHardDiskTemplate(int Id, string Manufacturer, string Model, uint Cylinders, uint Heads, uint Sectors, uint SectorSize);

    /// <summary>
    /// Receives the files a streaming extraction (<see cref="Chdman.ExtractCdToSink"/>,
    /// <see cref="Chdman.ExtractRawToSink"/>) produces instead of writing them to disk.
    /// Each file is one <see cref="BeginFile"/>, its bytes in order through
    /// <see cref="Write"/>, then <see cref="EndFile"/>. Called on the operation's
    /// thread; an exception thrown here cancels the operation and is rethrown.
    /// </summary>
    public abstract class ChdExtractSink
    {
        /// <param name="name">The path the file would have been written to.</param>
        public abstract void BeginFile(string name);

        /// <summary>The next block of the current file, valid only during the call. CD data blocks hold whole frames.</summary>
        public abstract void Write(ReadOnlySpan<byte> data);

        public abstract void EndFile();
    }

    /// <summary>
    /// One lib-chd-kit context. Operations block; run them on a worker thread
    /// (for example with Task.Run) to keep a UI responsive. An instance runs
    /// one operation at a time; use separate instances for parallel work.
    /// Every operation accepts a CancellationToken; a cancelled operation
    /// deletes its partial output and throws OperationCanceledException.
    /// </summary>
    public sealed class Chdman : IDisposable
    {
        readonly ContextHandle _ctx;
        readonly ProgressCallback _progressCallback; // referenced here so the GC keeps them alive
        readonly LogCallback _logCallback;
        readonly SinkCallback _sinkCallback;
        ChdExtractSink? _sink;
        int _busy;
        CancellationToken _token;
        ExceptionDispatchInfo? _callbackError;

        /// <summary>Receives progress reports (at most 10 per second) on the calling thread.</summary>
        public IProgress<ChdmanProgress>? Progress { get; set; }

        /// <summary>Receives chdman's informational lines, warnings and error details on the calling thread.</summary>
        public Action<ChdmanLogLevel, string>? Log { get; set; }

        /// <param name="threads">Compression worker threads; 0 = all processors.</param>
        public Chdman(int threads = 0)
        {
            Check(Native.chdman_context_create(out _ctx), null);
            _progressCallback = OnNativeProgress;
            _logCallback = OnNativeLog;
            _sinkCallback = OnNativeSink;
            Native.chdman_context_set_progress(_ctx, _progressCallback, IntPtr.Zero);
            Native.chdman_context_set_log(_ctx, _logCallback, IntPtr.Zero);
            Native.chdman_context_set_num_processors(_ctx, threads);
        }

        public static string Version => Native.Str(Native.chdman_version());
        public static int AbiVersion => Native.chdman_abi_version();

        /// <summary>Hard disk templates for <see cref="ChdHardDiskOptions.TemplateId"/>.</summary>
        public static IReadOnlyList<ChdHardDiskTemplate> Templates
        {
            get
            {
                var list = new List<ChdHardDiskTemplate>();
                nuint count = Native.chdman_template_count();
                for (nuint i = 0; i < count; i++)
                    if (Native.chdman_template_get(i, out var t) == ChdmanStatus.Ok)
                        list.Add(new ChdHardDiskTemplate((int)i, Native.Str(t.Manufacturer), Native.Str(t.Model), t.Cylinders, t.Heads, t.Sectors, t.SectorSize));
                return list;
            }
        }

        /// <summary>Cancels the running operation from any thread. A cancel just before an operation starts cancels that operation.</summary>
        public void Cancel()
        {
            try
            {
                if (!_ctx.IsClosed)
                    Native.chdman_context_cancel(_ctx);
            }
            catch (ObjectDisposedException)
            {
                // disposed concurrently: nothing left to cancel
            }
        }

        public void Dispose() => _ctx.Dispose();

        // ------------------------------------------------------------ info

        public ChdInfo Info(string input, bool hunkStats = false, string? inputParent = null, CancellationToken cancellationToken = default)
        {
            var o = new InfoOptions();
            Native.chdman_info_options_init(ref o);
            o.Input = input;
            o.InputParent = inputParent;
            o.HunkStats = hunkStats ? 1 : 0;
            InfoHandle? handle = null;
            Run(cancellationToken, () => Native.chdman_info_open(_ctx, ref o, out handle));
            using (handle!)
                return ReadInfo(handle!);
        }

        static ChdInfo ReadInfo(InfoHandle h)
        {
            var codecs = new List<string>();
            for (int slot = 0; slot < 4; slot++)
            {
                uint c = Native.chdman_info_codec(h, slot);
                if (c != 0)
                    codecs.Add(FourCC(c));
            }

            var metadata = new List<ChdMetadata>();
            nuint count = Native.chdman_info_metadata_count(h);
            for (nuint i = 0; i < count; i++)
            {
                if (Native.chdman_info_metadata_get(h, i, out var e) != ChdmanStatus.Ok)
                    continue;
                var data = new byte[(int)e.Length];
                if (e.Length > 0)
                    Marshal.Copy(e.Data, data, 0, data.Length);
                string tag = e.Tag0 == 0 ? "" : Encoding.ASCII.GetString(new[] { e.Tag0, e.Tag1, e.Tag2, e.Tag3 });
                metadata.Add(new ChdMetadata(e.Tag, tag, e.Index, (e.Flags & 1) != 0, data));
            }

            var stats = new List<ChdHunkStat>();
            nuint statCount = Native.chdman_info_hunk_stat_count(h);
            for (nuint i = 0; i < statCount; i++)
                if (Native.chdman_info_hunk_stat_get(h, i, out var s) == ChdmanStatus.Ok)
                    stats.Add(new ChdHunkStat(Native.Str(s.Name), s.Hunks));

            return new ChdInfo(
                Native.chdman_info_version(h),
                Native.chdman_info_logical_bytes(h),
                Native.chdman_info_hunk_bytes(h),
                Native.chdman_info_hunk_count(h),
                Native.chdman_info_unit_bytes(h),
                Native.chdman_info_unit_count(h),
                Native.chdman_info_file_bytes(h),
                Native.chdman_info_compressed(h) != 0,
                codecs,
                Native.Str(Native.chdman_info_compression_string(h)),
                Native.Str(Native.chdman_info_sha1(h)),
                Native.Str(Native.chdman_info_raw_sha1(h)),
                Native.Str(Native.chdman_info_parent_sha1(h)),
                Native.Str(Native.chdman_info_media_type(h)),
                metadata,
                stats);
        }

        // ---------------------------------------------------------- verify

        /// <summary>Checks the data against the header SHA-1s. The verdict is in the result; a mismatch doesn't throw.</summary>
        public ChdVerifyResult Verify(string input, bool fix = false, string? inputParent = null, CancellationToken cancellationToken = default)
        {
            var o = new VerifyOptions();
            Native.chdman_verify_options_init(ref o);
            o.Input = input;
            o.InputParent = inputParent;
            o.Fix = fix ? 1 : 0;
            var r = new VerifyResult();
            Native.chdman_verify_result_init(ref r);
            Run(cancellationToken, () => Native.chdman_verify(_ctx, ref o, ref r));
            return new ChdVerifyResult(r.Outcome, r.HeaderRawSha1 ?? "", r.ComputedRawSha1 ?? "", r.HeaderSha1 ?? "", r.ComputedSha1 ?? "");
        }

        // ---------------------------------------------------------- create

        public void CreateRaw(string input, string output, uint unitSize, ChdOutputSettings? settings = null, ChdRange? range = null, CancellationToken cancellationToken = default)
        {
            var o = new CreateRawOptions();
            Native.chdman_create_raw_options_init(ref o);
            o.Input = input;
            o.UnitSize = unitSize;
            o.Out = ToNative(output, settings);
            o.Range = ToNative(range);
            Run(cancellationToken, () => Native.chdman_create_raw(_ctx, ref o));
        }

        public void CreateHardDisk(string output, ChdHardDiskOptions? disk = null, ChdOutputSettings? settings = null, CancellationToken cancellationToken = default)
        {
            disk ??= new ChdHardDiskOptions();
            var o = new CreateHdOptions();
            Native.chdman_create_hd_options_init(ref o);
            o.Out = ToNative(output, settings);
            o.Input = disk.Input;
            o.Range = ToNative(disk.Range);
            o.Size = Native.Opt(disk.Size);
            o.TemplateId = disk.TemplateId ?? -1;
            o.Ident = disk.IdentFile;
            o.Cylinders = disk.Cylinders;
            o.Heads = disk.Heads;
            o.Sectors = disk.Sectors;
            o.SectorSize = disk.SectorSize;
            Run(cancellationToken, () => Native.chdman_create_hd(_ctx, ref o));
        }

        /// <param name="input">.cue, .toc, .gdi, .nrg or .iso</param>
        public void CreateCd(string input, string output, ChdOutputSettings? settings = null, CancellationToken cancellationToken = default)
        {
            var o = new CreateCdOptions();
            Native.chdman_create_cd_options_init(ref o);
            o.Input = input;
            o.Out = ToNative(output, settings);
            Run(cancellationToken, () => Native.chdman_create_cd(_ctx, ref o));
        }

        /// <param name="input">Raw image of 2048-byte sectors.</param>
        public void CreateDvd(string input, string output, ChdOutputSettings? settings = null, ChdRange? range = null, CancellationToken cancellationToken = default)
        {
            var o = new CreateDvdOptions();
            Native.chdman_create_dvd_options_init(ref o);
            o.Input = input;
            o.Out = ToNative(output, settings);
            o.Range = ToNative(range);
            Run(cancellationToken, () => Native.chdman_create_dvd(_ctx, ref o));
        }

        /// <param name="input">AVI with YUY2 video and 16-bit PCM audio.</param>
        public void CreateLaserdisc(string input, string output, ChdOutputSettings? settings = null, ulong? startFrame = null, ulong? lengthFrames = null, CancellationToken cancellationToken = default)
        {
            var o = new CreateLdOptions();
            Native.chdman_create_ld_options_init(ref o);
            o.Input = input;
            o.Out = ToNative(output, settings);
            o.StartFrame = Native.Opt(startFrame);
            o.LengthFrames = Native.Opt(lengthFrames);
            Run(cancellationToken, () => Native.chdman_create_ld(_ctx, ref o));
        }

        /// <summary>Copies a CHD, optionally recompressing it. Compression defaults to the detected media type's.</summary>
        public void Copy(string input, string output, ChdOutputSettings? settings = null, ChdRange? range = null, string? inputParent = null, CancellationToken cancellationToken = default)
        {
            var o = new CopyOptions();
            Native.chdman_copy_options_init(ref o);
            o.Input = input;
            o.InputParent = inputParent;
            o.Out = ToNative(output, settings);
            o.Range = ToNative(range);
            Run(cancellationToken, () => Native.chdman_copy(_ctx, ref o));
        }

        // --------------------------------------------------------- extract

        /// <summary>Raw extraction; also extracts hard disk and DVD CHDs.</summary>
        public void ExtractRaw(string input, string output, bool force = false, ChdRange? range = null, string? inputParent = null, CancellationToken cancellationToken = default)
        {
            var o = new ExtractRawOptions();
            Native.chdman_extract_raw_options_init(ref o);
            o.Input = input;
            o.InputParent = inputParent;
            o.Output = output;
            o.Force = force ? 1 : 0;
            o.Range = ToNative(range);
            Run(cancellationToken, () => Native.chdman_extract_raw(_ctx, ref o));
        }

        /// <summary>
        /// CD extraction. The output extension picks the format: .cue (CUE/BIN), .gdi (GDI), anything else (cdrdao TOC).
        /// <paramref name="outputBin"/> may contain %t or %02t for the track number, which <paramref name="splitBin"/> requires.
        /// </summary>
        public void ExtractCd(string input, string output, string? outputBin = null, bool splitBin = false, bool force = false, string? inputParent = null, CancellationToken cancellationToken = default)
        {
            var o = new ExtractCdOptions();
            Native.chdman_extract_cd_options_init(ref o);
            o.Input = input;
            o.InputParent = inputParent;
            o.Output = output;
            o.OutputBin = outputBin;
            o.SplitBin = splitBin ? 1 : 0;
            o.Force = force ? 1 : 0;
            Run(cancellationToken, () => Native.chdman_extract_cd(_ctx, ref o));
        }

        /// <summary>
        /// Like <see cref="ExtractRaw"/>, but passes the data to <paramref name="sink"/> as one file
        /// named <paramref name="outputName"/> instead of writing it. Nothing is written to disk.
        /// </summary>
        public void ExtractRawToSink(string input, string outputName, ChdExtractSink sink, ChdRange? range = null, string? inputParent = null, CancellationToken cancellationToken = default)
        {
            if (sink is null)
                throw new ArgumentNullException(nameof(sink));
            var o = new ExtractRawOptions();
            Native.chdman_extract_raw_options_init(ref o);
            o.Input = input;
            o.InputParent = inputParent;
            o.Output = outputName;
            o.Range = ToNative(range);
            RunWithSink(sink, cancellationToken, () => Native.chdman_extract_raw_to_sink(_ctx, ref o, _sinkCallback, IntPtr.Zero));
        }

        /// <summary>
        /// Like <see cref="ExtractCd"/>, but passes the files to <paramref name="sink"/> instead of writing them:
        /// first the TOC (<paramref name="output"/>, with the platform's line endings), then the data files in
        /// track order, byte-identical to what <see cref="ExtractCd"/> writes. <paramref name="output"/> and
        /// <paramref name="outputBin"/> pick the format and name the files; nothing is written to disk.
        /// </summary>
        public void ExtractCdToSink(string input, string output, ChdExtractSink sink, string? outputBin = null, bool splitBin = false, string? inputParent = null, CancellationToken cancellationToken = default)
        {
            if (sink is null)
                throw new ArgumentNullException(nameof(sink));
            var o = new ExtractCdOptions();
            Native.chdman_extract_cd_options_init(ref o);
            o.Input = input;
            o.InputParent = inputParent;
            o.Output = output;
            o.OutputBin = outputBin;
            o.SplitBin = splitBin ? 1 : 0;
            RunWithSink(sink, cancellationToken, () => Native.chdman_extract_cd_to_sink(_ctx, ref o, _sinkCallback, IntPtr.Zero));
        }

        /// <summary>Extracts a laserdisc CHD to an AVI.</summary>
        public void ExtractLaserdisc(string input, string output, bool force = false, ulong? startFrame = null, ulong? lengthFrames = null, string? inputParent = null, CancellationToken cancellationToken = default)
        {
            var o = new ExtractLdOptions();
            Native.chdman_extract_ld_options_init(ref o);
            o.Input = input;
            o.InputParent = inputParent;
            o.Output = output;
            o.Force = force ? 1 : 0;
            o.StartFrame = Native.Opt(startFrame);
            o.LengthFrames = Native.Opt(lengthFrames);
            Run(cancellationToken, () => Native.chdman_extract_ld(_ctx, ref o));
        }

        // -------------------------------------------------------- metadata
        // Adding and deleting only work on uncompressed CHDs (as in chdman).
        // Tags are up to 4 characters and padded with spaces.

        /// <summary>Adds text metadata, stored with a trailing NUL like chdman does.</summary>
        public void AddMetadata(string input, string tag, string text, uint index = 0, bool noChecksum = false, string? inputParent = null)
        {
            var o = MetadataOptions(input, tag, index, noChecksum, inputParent);
            o.ValueText = text;
            Run(default, () => Native.chdman_add_metadata(_ctx, ref o));
        }

        /// <summary>Adds binary metadata.</summary>
        public void AddMetadata(string input, string tag, byte[] data, uint index = 0, bool noChecksum = false, string? inputParent = null)
        {
            if (data is null)
                throw new ArgumentNullException(nameof(data));
            var pin = GCHandle.Alloc(data, GCHandleType.Pinned);
            try
            {
                var o = MetadataOptions(input, tag, index, noChecksum, inputParent);
                // the C API treats a NULL pointer as "no data", so point an empty array at a dummy byte
                o.ValueData = data.Length > 0 ? pin.AddrOfPinnedObject() : (IntPtr)1;
                o.ValueLength = (nuint)data.Length;
                Run(default, () => Native.chdman_add_metadata(_ctx, ref o));
            }
            finally
            {
                pin.Free();
            }
        }

        /// <summary>Adds the contents of a file as metadata.</summary>
        public void AddMetadataFromFile(string input, string tag, string valueFile, uint index = 0, bool noChecksum = false, string? inputParent = null)
        {
            var o = MetadataOptions(input, tag, index, noChecksum, inputParent);
            o.ValueFile = valueFile;
            Run(default, () => Native.chdman_add_metadata(_ctx, ref o));
        }

        public void DeleteMetadata(string input, string tag, uint index = 0, string? inputParent = null)
            => Run(default, () => Native.chdman_del_metadata(_ctx, input, inputParent, tag, index));

        public byte[] GetMetadata(string input, string tag, uint index = 0, string? inputParent = null)
        {
            IntPtr data = IntPtr.Zero;
            nuint length = 0;
            try
            {
                Run(default, () => Native.chdman_get_metadata(_ctx, input, inputParent, tag, index, out data, out length));
                var bytes = new byte[(int)length];
                if (length > 0)
                    Marshal.Copy(data, bytes, 0, bytes.Length);
                return bytes;
            }
            finally
            {
                if (data != IntPtr.Zero)
                    Native.chdman_free(data);
            }
        }

        /// <summary>Writes one metadata item to a file.</summary>
        public void DumpMetadata(string input, string tag, string output, uint index = 0, bool force = false, string? inputParent = null)
            => Run(default, () => Native.chdman_dump_metadata(_ctx, input, inputParent, tag, index, output, force ? 1 : 0));

        static AddMetadataOptions MetadataOptions(string input, string tag, uint index, bool noChecksum, string? inputParent)
        {
            var o = new AddMetadataOptions();
            Native.chdman_add_metadata_options_init(ref o);
            o.Input = input;
            o.InputParent = inputParent;
            o.Tag = tag;
            o.Index = index;
            o.NoChecksum = noChecksum ? 1 : 0;
            return o;
        }

        // ------------------------------------------------------- internals

        delegate ChdmanStatus NativeCall();

        void Run(CancellationToken token, NativeCall call)
        {
            if (Interlocked.Exchange(ref _busy, 1) != 0)
                throw new InvalidOperationException("This Chdman instance is already running an operation; use one instance per concurrent operation.");
            try
            {
                ObjectDisposedException.ThrowIf(_ctx.IsClosed, this);
                token.ThrowIfCancellationRequested();
                _token = token;
                _callbackError = null;
                ChdmanStatus status = call();
                _callbackError?.Throw();
                if (status == ChdmanStatus.Cancelled)
                    token.ThrowIfCancellationRequested();
                Check(status, _ctx);
            }
            finally
            {
                _token = default;
                _callbackError = null;
                Volatile.Write(ref _busy, 0);
            }
        }

        static void Check(ChdmanStatus status, ContextHandle? ctx)
        {
            if (status == ChdmanStatus.Ok)
                return;
            string message = ctx is { IsInvalid: false, IsClosed: false } ? Native.Str(Native.chdman_last_error(ctx)) : "";
            if (message.Length == 0)
                message = Native.Str(Native.chdman_status_string(status));
            throw new ChdmanException(status, message);
        }

        // Callbacks run on the operation's thread. Exceptions must not cross
        // into native code: they cancel the operation and are rethrown by Run.
        int OnNativeProgress(IntPtr user, ChdmanPhase phase, double fraction, double ratio)
        {
            if (_callbackError != null)
                return 1;
            try
            {
                Progress?.Report(new ChdmanProgress(phase, fraction, ratio));
                return _token.IsCancellationRequested ? 1 : 0;
            }
            catch (Exception ex)
            {
                _callbackError = ExceptionDispatchInfo.Capture(ex);
                return 1;
            }
        }

        void RunWithSink(ChdExtractSink sink, CancellationToken token, NativeCall call)
        {
            // Run guards against concurrent use, so _sink is only set inside it
            Run(token, () =>
            {
                _sink = sink;
                try { return call(); }
                finally { _sink = null; }
            });
        }

        unsafe int OnNativeSink(IntPtr user, SinkEvent ev, IntPtr name, IntPtr data, nuint length)
        {
            if (_callbackError != null || _sink is null)
                return 1;
            try
            {
                switch (ev)
                {
                    case SinkEvent.BeginFile:
                        _sink.BeginFile(Native.Str(name));
                        break;
                    case SinkEvent.Data:
                        _sink.Write(new ReadOnlySpan<byte>((void*)data, checked((int)length)));
                        break;
                    case SinkEvent.EndFile:
                        _sink.EndFile();
                        break;
                }
                return _token.IsCancellationRequested ? 1 : 0;
            }
            catch (Exception ex)
            {
                _callbackError = ExceptionDispatchInfo.Capture(ex);
                return 1;
            }
        }

        void OnNativeLog(IntPtr user, ChdmanLogLevel level, IntPtr message)
        {
            if (_callbackError != null)
                return;
            try
            {
                Log?.Invoke(level, Native.Str(message));
            }
            catch (Exception ex)
            {
                _callbackError = ExceptionDispatchInfo.Capture(ex);
            }
        }

        static OutputOptions ToNative(string output, ChdOutputSettings? s) => new OutputOptions
        {
            Output = output,
            OutputParent = s?.Parent,
            Force = s?.Force == true ? 1 : 0,
            HunkSize = s?.HunkSize ?? 0,
            Compression = s?.Compression,
        };

        static RangeOptions ToNative(ChdRange? r) => new RangeOptions
        {
            StartByte = Native.Opt(r?.StartByte),
            StartHunk = Native.Opt(r?.StartHunk),
            LengthBytes = Native.Opt(r?.LengthBytes),
            LengthHunks = Native.Opt(r?.LengthHunks),
        };

        static string FourCC(uint c) =>
            new string(new[] { (char)((c >> 24) & 0xff), (char)((c >> 16) & 0xff), (char)((c >> 8) & 0xff), (char)(c & 0xff) });
    }
}
