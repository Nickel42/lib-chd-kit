// Raw P/Invoke layer for lib-chd-kit (chdman.h). Struct layouts mirror the C
// header exactly; the public, idiomatic API lives in Chdman.cs.
// SPDX-License-Identifier: BSD-3-Clause

using System;
using System.Runtime.InteropServices;

namespace LibChdKit.Interop
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int ProgressCallback(IntPtr user, ChdmanPhase phase, double fraction, double ratio);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void LogCallback(IntPtr user, ChdmanLogLevel level, IntPtr message);

    internal enum SinkEvent : int { BeginFile = 0, Data = 1, EndFile = 2 }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int SinkCallback(IntPtr user, SinkEvent ev, IntPtr name, IntPtr data, nuint length);

    /// <summary>Owns a chdman_context. Passing it to a native call keeps it alive for the call's duration.</summary>
    internal sealed class ContextHandle : SafeHandle
    {
        public ContextHandle() : base(IntPtr.Zero, ownsHandle: true) { }
        public override bool IsInvalid => handle == IntPtr.Zero;
        protected override bool ReleaseHandle()
        {
            Native.chdman_context_destroy(handle);
            return true;
        }
    }

    /// <summary>Owns a chdman_info.</summary>
    internal sealed class InfoHandle : SafeHandle
    {
        public InfoHandle() : base(IntPtr.Zero, ownsHandle: true) { }
        public override bool IsInvalid => handle == IntPtr.Zero;
        protected override bool ReleaseHandle()
        {
            Native.chdman_info_free(handle);
            return true;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OutputOptions
    {
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Output;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? OutputParent;
        public int Force;
        public uint HunkSize;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Compression;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct RangeOptions
    {
        public ulong StartByte, StartHunk, LengthBytes, LengthHunks; // Native.Unset when unused
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct InfoOptions
    {
        public nuint StructSize;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? InputParent;
        public int HunkStats;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct VerifyOptions
    {
        public nuint StructSize;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? InputParent;
        public int Fix;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    internal struct VerifyResult
    {
        public nuint StructSize;
        public ChdVerifyOutcome Outcome;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 41)] public string HeaderRawSha1;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 41)] public string ComputedRawSha1;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 41)] public string HeaderSha1;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 41)] public string ComputedSha1;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CreateRawOptions
    {
        public nuint StructSize;
        public OutputOptions Out;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        public RangeOptions Range;
        public uint UnitSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CreateHdOptions
    {
        public nuint StructSize;
        public OutputOptions Out;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        public RangeOptions Range;
        public ulong Size;
        public int TemplateId;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Ident;
        public uint Cylinders, Heads, Sectors, SectorSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CreateCdOptions
    {
        public nuint StructSize;
        public OutputOptions Out;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CreateDvdOptions
    {
        public nuint StructSize;
        public OutputOptions Out;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        public RangeOptions Range;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CreateLdOptions
    {
        public nuint StructSize;
        public OutputOptions Out;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        public ulong StartFrame, LengthFrames;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CopyOptions
    {
        public nuint StructSize;
        public OutputOptions Out;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? InputParent;
        public RangeOptions Range;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ExtractRawOptions
    {
        public nuint StructSize;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? InputParent;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Output;
        public int Force;
        public RangeOptions Range;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ExtractCdOptions
    {
        public nuint StructSize;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? InputParent;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Output;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? OutputBin;
        public int SplitBin;
        public int Force;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ExtractLdOptions
    {
        public nuint StructSize;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? InputParent;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Output;
        public int Force;
        public ulong StartFrame, LengthFrames;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct AddMetadataOptions
    {
        public nuint StructSize;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Input;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? InputParent;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? Tag;
        public uint Index;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? ValueText;
        [MarshalAs(UnmanagedType.LPUTF8Str)] public string? ValueFile;
        public IntPtr ValueData;
        public nuint ValueLength;
        public int NoChecksum;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct MetadataEntry
    {
        public uint Tag;
        public byte Tag0, Tag1, Tag2, Tag3, TagNul;
        public uint Index;
        public byte Flags;
        public IntPtr Data;
        public nuint Length;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct HunkStat
    {
        public IntPtr Name;
        public uint Hunks;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct HdTemplate
    {
        public IntPtr Manufacturer, Model; // static UTF-8 strings
        public uint Cylinders, Heads, Sectors, SectorSize;
    }

    internal static class Native
    {
        // resolves to chdkit.dll on Windows and libchdkit.dylib on macOS
        const string Lib = "chdkit";
        public const ulong Unset = ulong.MaxValue;

        [DllImport(Lib)] public static extern IntPtr chdman_version();
        [DllImport(Lib)] public static extern int chdman_abi_version();
        [DllImport(Lib)] public static extern IntPtr chdman_status_string(ChdmanStatus status);

        [DllImport(Lib)] public static extern ChdmanStatus chdman_context_create(out ContextHandle ctx);
        [DllImport(Lib)] public static extern void chdman_context_destroy(IntPtr ctx);
        [DllImport(Lib)] public static extern void chdman_context_set_progress(ContextHandle ctx, ProgressCallback? fn, IntPtr user);
        [DllImport(Lib)] public static extern void chdman_context_set_log(ContextHandle ctx, LogCallback? fn, IntPtr user);
        [DllImport(Lib)] public static extern void chdman_context_set_num_processors(ContextHandle ctx, int count);
        [DllImport(Lib)] public static extern void chdman_context_cancel(ContextHandle ctx);
        [DllImport(Lib)] public static extern IntPtr chdman_last_error(ContextHandle ctx);
        [DllImport(Lib)] public static extern void chdman_free(IntPtr ptr);

        [DllImport(Lib)] public static extern void chdman_info_options_init(ref InfoOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_info_open(ContextHandle ctx, [In] ref InfoOptions o, out InfoHandle info);
        [DllImport(Lib)] public static extern void chdman_info_free(IntPtr info);
        [DllImport(Lib)] public static extern uint chdman_info_version(InfoHandle info);
        [DllImport(Lib)] public static extern ulong chdman_info_logical_bytes(InfoHandle info);
        [DllImport(Lib)] public static extern uint chdman_info_hunk_bytes(InfoHandle info);
        [DllImport(Lib)] public static extern uint chdman_info_hunk_count(InfoHandle info);
        [DllImport(Lib)] public static extern uint chdman_info_unit_bytes(InfoHandle info);
        [DllImport(Lib)] public static extern ulong chdman_info_unit_count(InfoHandle info);
        [DllImport(Lib)] public static extern ulong chdman_info_file_bytes(InfoHandle info);
        [DllImport(Lib)] public static extern int chdman_info_compressed(InfoHandle info);
        [DllImport(Lib)] public static extern uint chdman_info_codec(InfoHandle info, int slot);
        [DllImport(Lib)] public static extern IntPtr chdman_info_compression_string(InfoHandle info);
        [DllImport(Lib)] public static extern IntPtr chdman_info_sha1(InfoHandle info);
        [DllImport(Lib)] public static extern IntPtr chdman_info_raw_sha1(InfoHandle info);
        [DllImport(Lib)] public static extern IntPtr chdman_info_parent_sha1(InfoHandle info);
        [DllImport(Lib)] public static extern IntPtr chdman_info_media_type(InfoHandle info);
        [DllImport(Lib)] public static extern nuint chdman_info_metadata_count(InfoHandle info);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_info_metadata_get(InfoHandle info, nuint i, out MetadataEntry e);
        [DllImport(Lib)] public static extern nuint chdman_info_hunk_stat_count(InfoHandle info);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_info_hunk_stat_get(InfoHandle info, nuint i, out HunkStat s);

        [DllImport(Lib)] public static extern void chdman_verify_options_init(ref VerifyOptions o);
        [DllImport(Lib)] public static extern void chdman_verify_result_init(ref VerifyResult r);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_verify(ContextHandle ctx, [In] ref VerifyOptions o, ref VerifyResult r);

        [DllImport(Lib)] public static extern void chdman_create_raw_options_init(ref CreateRawOptions o);
        [DllImport(Lib)] public static extern void chdman_create_hd_options_init(ref CreateHdOptions o);
        [DllImport(Lib)] public static extern void chdman_create_cd_options_init(ref CreateCdOptions o);
        [DllImport(Lib)] public static extern void chdman_create_dvd_options_init(ref CreateDvdOptions o);
        [DllImport(Lib)] public static extern void chdman_create_ld_options_init(ref CreateLdOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_create_raw(ContextHandle ctx, [In] ref CreateRawOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_create_hd(ContextHandle ctx, [In] ref CreateHdOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_create_cd(ContextHandle ctx, [In] ref CreateCdOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_create_dvd(ContextHandle ctx, [In] ref CreateDvdOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_create_ld(ContextHandle ctx, [In] ref CreateLdOptions o);

        [DllImport(Lib)] public static extern void chdman_copy_options_init(ref CopyOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_copy(ContextHandle ctx, [In] ref CopyOptions o);

        [DllImport(Lib)] public static extern void chdman_extract_raw_options_init(ref ExtractRawOptions o);
        [DllImport(Lib)] public static extern void chdman_extract_cd_options_init(ref ExtractCdOptions o);
        [DllImport(Lib)] public static extern void chdman_extract_ld_options_init(ref ExtractLdOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_extract_raw(ContextHandle ctx, [In] ref ExtractRawOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_extract_cd(ContextHandle ctx, [In] ref ExtractCdOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_extract_ld(ContextHandle ctx, [In] ref ExtractLdOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_extract_raw_to_sink(ContextHandle ctx, [In] ref ExtractRawOptions o, SinkCallback fn, IntPtr user);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_extract_cd_to_sink(ContextHandle ctx, [In] ref ExtractCdOptions o, SinkCallback fn, IntPtr user);

        [DllImport(Lib)] public static extern void chdman_add_metadata_options_init(ref AddMetadataOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_add_metadata(ContextHandle ctx, [In] ref AddMetadataOptions o);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_del_metadata(ContextHandle ctx, [MarshalAs(UnmanagedType.LPUTF8Str)] string input, [MarshalAs(UnmanagedType.LPUTF8Str)] string? inputParent, [MarshalAs(UnmanagedType.LPUTF8Str)] string tag, uint index);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_dump_metadata(ContextHandle ctx, [MarshalAs(UnmanagedType.LPUTF8Str)] string input, [MarshalAs(UnmanagedType.LPUTF8Str)] string? inputParent, [MarshalAs(UnmanagedType.LPUTF8Str)] string tag, uint index, [MarshalAs(UnmanagedType.LPUTF8Str)] string output, int force);
        [DllImport(Lib)] public static extern ChdmanStatus chdman_get_metadata(ContextHandle ctx, [MarshalAs(UnmanagedType.LPUTF8Str)] string input, [MarshalAs(UnmanagedType.LPUTF8Str)] string? inputParent, [MarshalAs(UnmanagedType.LPUTF8Str)] string tag, uint index, out IntPtr data, out nuint length);

        [DllImport(Lib)] public static extern nuint chdman_template_count();
        [DllImport(Lib)] public static extern ChdmanStatus chdman_template_get(nuint id, out HdTemplate t);

        public static string Str(IntPtr p) => Marshal.PtrToStringUTF8(p) ?? "";
        public static ulong Opt(ulong? v) => v ?? Unset;
    }
}
