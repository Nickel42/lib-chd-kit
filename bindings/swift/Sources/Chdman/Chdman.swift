// Swift API for lib-chd-kit: MAME's CHD tools (chdman) as a library.
// SPDX-License-Identifier: BSD-3-Clause

@_exported import CChdman
import Foundation

/// A lib-chd-kit call failed.
public struct ChdmanError: Error, CustomStringConvertible, Sendable {
    public enum Status: Int, Sendable {
        case invalidArgument = 1, fileExists, fileOpen, io, chd, unsupported, notFound, cancelled, outOfMemory, `internal`, abiMismatch
        case unknown = -1
    }
    public let status: Status
    public let message: String
    public var description: String { message }
}

/// Settings shared by every create operation and `copy`.
public struct ChdOutputSettings: Sendable {
    /// nil = default for the media type, "none", or up to four comma-separated codecs ("lzma,zlib,huff,flac").
    public var compression: String?
    /// 0 = default for the media type.
    public var hunkSize: UInt32
    /// Optional parent CHD to deduplicate against.
    public var parent: String?
    /// Overwrite an existing output file.
    public var force: Bool

    public init(compression: String? = nil, hunkSize: UInt32 = 0, parent: String? = nil, force: Bool = false) {
        self.compression = compression
        self.hunkSize = hunkSize
        self.parent = parent
        self.force = force
    }
}

/// Input range. Set at most one start and at most one length.
public struct ChdRange: Sendable {
    public var startByte: UInt64?
    public var startHunk: UInt64?
    public var lengthBytes: UInt64?
    public var lengthHunks: UInt64?

    public init(startByte: UInt64? = nil, startHunk: UInt64? = nil, lengthBytes: UInt64? = nil, lengthHunks: UInt64? = nil) {
        self.startByte = startByte
        self.startHunk = startHunk
        self.lengthBytes = lengthBytes
        self.lengthHunks = lengthHunks
    }
}

/// Hard disk parameters for `createHardDisk`.
public struct ChdHardDiskOptions: Sendable {
    /// Raw disk image. nil creates a blank (zeroed, uncompressed) disk.
    public var input: String?
    /// Blank disk size in bytes (only without `input`).
    public var size: UInt64?
    /// Index into `Chdman.templates`.
    public var templateID: Int?
    /// Optional IDENTIFY data file; CHS is read from it.
    public var identFile: String?
    /// Set all three, or leave them 0 to have them guessed.
    public var cylinders: UInt32 = 0
    public var heads: UInt32 = 0
    public var sectors: UInt32 = 0
    /// 0 = 512 (or the template's or parent's).
    public var sectorSize: UInt32 = 0
    public var range: ChdRange?

    public init(input: String? = nil, size: UInt64? = nil, templateID: Int? = nil, identFile: String? = nil,
                cylinders: UInt32 = 0, heads: UInt32 = 0, sectors: UInt32 = 0, sectorSize: UInt32 = 0, range: ChdRange? = nil) {
        self.input = input
        self.size = size
        self.templateID = templateID
        self.identFile = identFile
        self.cylinders = cylinders
        self.heads = heads
        self.sectors = sectors
        self.sectorSize = sectorSize
        self.range = range
    }
}

public struct ChdMetadata: Sendable {
    public let tag: UInt32
    public let tagString: String
    public let index: UInt32
    public let checksummed: Bool
    public let data: Data
    /// The data as UTF-8 text, without the trailing NUL chdman stores.
    public var text: String {
        String(decoding: data.last == 0 ? data.dropLast() : data, as: UTF8.self)
    }
}

public struct ChdHunkStat: Sendable {
    public let name: String
    public let hunks: UInt32
}

public struct ChdInfo: Sendable {
    public let version: UInt32
    public let logicalBytes: UInt64
    public let hunkBytes: UInt32
    public let hunkCount: UInt32
    public let unitBytes: UInt32
    public let unitCount: UInt64
    public let fileBytes: UInt64
    public let isCompressed: Bool
    public let codecs: [String]
    public let compression: String
    public let sha1: String
    public let rawSHA1: String
    public let parentSHA1: String
    /// "hd", "cd", "gd", "dvd", "av" or "raw"
    public let mediaType: String
    public let metadata: [ChdMetadata]
    public let hunkStats: [ChdHunkStat]
}

public struct ChdVerifyResult: Sendable {
    public enum Outcome: Int, Sendable { case ok = 0, rawMismatch, overallMismatch, fixed, notApplicable }
    public let outcome: Outcome
    public let headerRawSHA1: String
    public let computedRawSHA1: String
    public let headerSHA1: String
    public let computedSHA1: String
}

public struct ChdHardDiskTemplate: Sendable {
    public let id: Int
    public let manufacturer: String
    public let model: String
    public let cylinders: UInt32
    public let heads: UInt32
    public let sectors: UInt32
    public let sectorSize: UInt32
}

public struct ChdProgress: Sendable {
    public enum Phase: Int, Sendable { case walkingParent = 0, compressing, verifying, extracting, analyzing }
    public let phase: Phase
    /// 0...1
    public let fraction: Double
    /// compressed/uncompressed while compressing, otherwise 0
    public let ratio: Double
}

public enum ChdLogLevel: Int, Sendable { case info = 0, warning, error }

/// One lib-chd-kit context.
///
/// Operations block. From Swift concurrency, wrap them in `run`, which
/// moves them off the calling actor and cancels them when the task is
/// cancelled. An instance runs one operation at a time; use separate
/// instances for parallel work. A cancelled operation deletes its partial
/// output.
public final class Chdman: @unchecked Sendable {
    private let ctx: OpaquePointer
    private let lock = NSLock()
    private var busy = false
    private var taskCancelled = false   // the task of the current `run` was cancelled
    private var runGeneration = 0       // identifies the current `run`, so a late onCancel is ignored

    /// Called on the operation's thread, at most 10 times per second. Return true to cancel.
    public var onProgress: ((ChdProgress) -> Bool)?
    /// chdman's informational lines, warnings and error details, on the operation's thread.
    public var onLog: ((ChdLogLevel, String) -> Void)?

    /// - Parameter threads: compression worker threads; 0 = all processors.
    public init(threads: Int = 0) throws {
        var c: OpaquePointer?
        let st = chdman_context_create(&c)
        guard st == CHDMAN_OK, let c else {
            throw ChdmanError(status: .outOfMemory, message: String(cString: chdman_status_string(st)))
        }
        ctx = c
        let me = Unmanaged.passUnretained(self).toOpaque()
        chdman_context_set_progress(ctx, { user, phase, fraction, ratio in
            let this = Unmanaged<Chdman>.fromOpaque(user!).takeUnretainedValue()
            return this.progress(phase: Int(phase.rawValue), fraction: fraction, ratio: ratio) ? 1 : 0
        }, me)
        chdman_context_set_log(ctx, { user, level, message in
            let this = Unmanaged<Chdman>.fromOpaque(user!).takeUnretainedValue()
            this.onLog?(ChdLogLevel(rawValue: Int(level.rawValue)) ?? .warning, message.map { String(cString: $0) } ?? "")
        }, me)
        chdman_context_set_num_processors(ctx, Int32(threads))
    }

    deinit { chdman_context_destroy(ctx) }

    public static var version: String { String(cString: chdman_version()) }
    public static var abiVersion: Int { Int(chdman_abi_version()) }

    public static var templates: [ChdHardDiskTemplate] {
        (0..<chdman_template_count()).compactMap { id in
            var t = chdman_hd_template()
            guard chdman_template_get(id, &t) == CHDMAN_OK else { return nil }
            return ChdHardDiskTemplate(id: id, manufacturer: String(cString: t.manufacturer), model: String(cString: t.model),
                                       cylinders: t.cylinders, heads: t.heads, sectors: t.sectors, sectorSize: t.sector_size)
        }
    }

    /// Cancels the running operation; callable from any thread. A cancel just before an operation starts cancels that operation.
    public func cancel() { chdman_context_cancel(ctx) }

    /// Runs blocking work on this instance off the current actor. Cancelling the task cancels the operation.
    ///
    ///     try await chdman.run { try $0.createCD(input: cue, output: chd) }
    public func run<T: Sendable>(_ body: @escaping @Sendable (Chdman) throws -> T) async throws -> T {
        try Task.checkCancellation()
        let generation: Int = lock.locked {
            runGeneration += 1
            taskCancelled = false
            return runGeneration
        }
        return try await withTaskCancellationHandler {
            try await Task.detached { [self] in
                defer {
                    lock.locked {
                        // retire this generation so a later onCancel for it is ignored
                        if runGeneration == generation {
                            runGeneration += 1
                            taskCancelled = false
                        }
                    }
                }
                return try body(self)
            }.value
        } onCancel: { [self] in
            // read by the progress callback, so a cancel arriving after the
            // operation finished can't cancel the next one
            lock.locked {
                if runGeneration == generation { taskCancelled = true }
            }
        }
    }

    // MARK: info / verify

    public func info(_ input: String, hunkStats: Bool = false, inputParent: String? = nil) throws -> ChdInfo {
        let s = CStrings()
        var o = chdman_info_options()
        chdman_info_options_init(&o)
        o.input = s[input]
        o.input_parent = s[inputParent]
        o.hunk_stats = hunkStats ? 1 : 0
        var handle: OpaquePointer?
        try perform(s) { chdman_info_open(ctx, &o, &handle) }
        guard let handle else { throw ChdmanError(status: .internal, message: "no info handle") }
        defer { chdman_info_free(handle) }

        var metadata: [ChdMetadata] = []
        for i in 0..<chdman_info_metadata_count(handle) {
            var e = chdman_metadata_entry()
            guard chdman_info_metadata_get(handle, i, &e) == CHDMAN_OK else { continue }
            let tag = withUnsafeBytes(of: e.tag_string) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
            let data = e.data.map { Data(bytes: $0, count: e.length) } ?? Data()
            metadata.append(ChdMetadata(tag: e.tag, tagString: tag, index: e.index, checksummed: (e.flags & 1) != 0, data: data))
        }
        var stats: [ChdHunkStat] = []
        for i in 0..<chdman_info_hunk_stat_count(handle) {
            var st = chdman_hunk_stat()
            if chdman_info_hunk_stat_get(handle, i, &st) == CHDMAN_OK {
                stats.append(ChdHunkStat(name: String(cString: st.name), hunks: st.hunks))
            }
        }
        let codecs = (0..<4).compactMap { slot -> String? in
            let c = chdman_info_codec(handle, Int32(slot))
            guard c != 0 else { return nil }
            return String(decoding: [UInt8(c >> 24 & 0xff), UInt8(c >> 16 & 0xff), UInt8(c >> 8 & 0xff), UInt8(c & 0xff)], as: UTF8.self)
        }
        return ChdInfo(
            version: chdman_info_version(handle),
            logicalBytes: chdman_info_logical_bytes(handle),
            hunkBytes: chdman_info_hunk_bytes(handle),
            hunkCount: chdman_info_hunk_count(handle),
            unitBytes: chdman_info_unit_bytes(handle),
            unitCount: chdman_info_unit_count(handle),
            fileBytes: chdman_info_file_bytes(handle),
            isCompressed: chdman_info_compressed(handle) != 0,
            codecs: codecs,
            compression: String(cString: chdman_info_compression_string(handle)),
            sha1: String(cString: chdman_info_sha1(handle)),
            rawSHA1: String(cString: chdman_info_raw_sha1(handle)),
            parentSHA1: String(cString: chdman_info_parent_sha1(handle)),
            mediaType: String(cString: chdman_info_media_type(handle)),
            metadata: metadata,
            hunkStats: stats)
    }

    /// Checks the data against the header SHA-1s. The verdict is in the result; a mismatch doesn't throw.
    public func verify(_ input: String, fix: Bool = false, inputParent: String? = nil) throws -> ChdVerifyResult {
        let s = CStrings()
        var o = chdman_verify_options()
        chdman_verify_options_init(&o)
        o.input = s[input]
        o.input_parent = s[inputParent]
        o.fix = fix ? 1 : 0
        var r = chdman_verify_result()
        chdman_verify_result_init(&r)
        try perform(s) { chdman_verify(ctx, &o, &r) }
        return ChdVerifyResult(
            outcome: ChdVerifyResult.Outcome(rawValue: Int(r.outcome.rawValue)) ?? .notApplicable,
            headerRawSHA1: fixedString(r.header_raw_sha1),
            computedRawSHA1: fixedString(r.computed_raw_sha1),
            headerSHA1: fixedString(r.header_sha1),
            computedSHA1: fixedString(r.computed_sha1))
    }

    // MARK: create / copy

    public func createRaw(input: String, output: String, unitSize: UInt32, settings: ChdOutputSettings = .init(), range: ChdRange? = nil) throws {
        let s = CStrings()
        var o = chdman_create_raw_options()
        chdman_create_raw_options_init(&o)
        o.input = s[input]
        o.unit_size = unitSize
        apply(&o.out, output, settings, s)
        apply(&o.range, range)
        try perform(s) { chdman_create_raw(ctx, &o) }
    }

    public func createHardDisk(output: String, disk: ChdHardDiskOptions = .init(), settings: ChdOutputSettings = .init()) throws {
        let s = CStrings()
        var o = chdman_create_hd_options()
        chdman_create_hd_options_init(&o)
        apply(&o.out, output, settings, s)
        o.input = s[disk.input]
        apply(&o.range, disk.range)
        o.size = disk.size ?? UInt64.max
        o.template_id = Int32(disk.templateID ?? -1)
        o.ident = s[disk.identFile]
        o.cylinders = disk.cylinders
        o.heads = disk.heads
        o.sectors = disk.sectors
        o.sector_size = disk.sectorSize
        try perform(s) { chdman_create_hd(ctx, &o) }
    }

    /// - Parameter input: .cue, .toc, .gdi, .nrg or .iso
    public func createCD(input: String, output: String, settings: ChdOutputSettings = .init()) throws {
        let s = CStrings()
        var o = chdman_create_cd_options()
        chdman_create_cd_options_init(&o)
        o.input = s[input]
        apply(&o.out, output, settings, s)
        try perform(s) { chdman_create_cd(ctx, &o) }
    }

    /// - Parameter input: raw image of 2048-byte sectors
    public func createDVD(input: String, output: String, settings: ChdOutputSettings = .init(), range: ChdRange? = nil) throws {
        let s = CStrings()
        var o = chdman_create_dvd_options()
        chdman_create_dvd_options_init(&o)
        o.input = s[input]
        apply(&o.out, output, settings, s)
        apply(&o.range, range)
        try perform(s) { chdman_create_dvd(ctx, &o) }
    }

    /// - Parameter input: AVI with YUY2 video and 16-bit PCM audio
    public func createLaserdisc(input: String, output: String, settings: ChdOutputSettings = .init(), startFrame: UInt64? = nil, lengthFrames: UInt64? = nil) throws {
        let s = CStrings()
        var o = chdman_create_ld_options()
        chdman_create_ld_options_init(&o)
        o.input = s[input]
        apply(&o.out, output, settings, s)
        o.start_frame = startFrame ?? UInt64.max
        o.length_frames = lengthFrames ?? UInt64.max
        try perform(s) { chdman_create_ld(ctx, &o) }
    }

    /// Copies a CHD, optionally recompressing it. Compression defaults to the detected media type's.
    public func copy(input: String, output: String, settings: ChdOutputSettings = .init(), range: ChdRange? = nil, inputParent: String? = nil) throws {
        let s = CStrings()
        var o = chdman_copy_options()
        chdman_copy_options_init(&o)
        o.input = s[input]
        o.input_parent = s[inputParent]
        apply(&o.out, output, settings, s)
        apply(&o.range, range)
        try perform(s) { chdman_copy(ctx, &o) }
    }

    // MARK: extract

    /// Raw extraction; also extracts hard disk and DVD CHDs.
    public func extractRaw(input: String, output: String, force: Bool = false, range: ChdRange? = nil, inputParent: String? = nil) throws {
        let s = CStrings()
        var o = chdman_extract_raw_options()
        chdman_extract_raw_options_init(&o)
        o.input = s[input]
        o.input_parent = s[inputParent]
        o.output = s[output]
        o.force = force ? 1 : 0
        apply(&o.range, range)
        try perform(s) { chdman_extract_raw(ctx, &o) }
    }

    /// The output extension picks the format: .cue (CUE/BIN), .gdi (GDI), anything else (cdrdao TOC).
    /// `outputBin` may contain %t or %02t for the track number, which `splitBin` requires.
    public func extractCD(input: String, output: String, outputBin: String? = nil, splitBin: Bool = false, force: Bool = false, inputParent: String? = nil) throws {
        let s = CStrings()
        var o = chdman_extract_cd_options()
        chdman_extract_cd_options_init(&o)
        o.input = s[input]
        o.input_parent = s[inputParent]
        o.output = s[output]
        o.output_bin = s[outputBin]
        o.split_bin = splitBin ? 1 : 0
        o.force = force ? 1 : 0
        try perform(s) { chdman_extract_cd(ctx, &o) }
    }

    public func extractLaserdisc(input: String, output: String, force: Bool = false, startFrame: UInt64? = nil, lengthFrames: UInt64? = nil, inputParent: String? = nil) throws {
        let s = CStrings()
        var o = chdman_extract_ld_options()
        chdman_extract_ld_options_init(&o)
        o.input = s[input]
        o.input_parent = s[inputParent]
        o.output = s[output]
        o.force = force ? 1 : 0
        o.start_frame = startFrame ?? UInt64.max
        o.length_frames = lengthFrames ?? UInt64.max
        try perform(s) { chdman_extract_ld(ctx, &o) }
    }

    // MARK: metadata
    // Adding and deleting only work on uncompressed CHDs (as in chdman).
    // Tags are up to 4 characters and padded with spaces.

    /// Adds text metadata, stored with a trailing NUL like chdman does.
    public func addMetadata(input: String, tag: String, text: String, index: UInt32 = 0, noChecksum: Bool = false, inputParent: String? = nil) throws {
        let s = CStrings()
        var o = metadataOptions(input, tag, index, noChecksum, inputParent, s)
        o.value_text = s[text]
        try perform(s) { chdman_add_metadata(ctx, &o) }
    }

    /// Adds binary metadata.
    public func addMetadata(input: String, tag: String, data: Data, index: UInt32 = 0, noChecksum: Bool = false, inputParent: String? = nil) throws {
        let s = CStrings()
        var o = metadataOptions(input, tag, index, noChecksum, inputParent, s)
        let bytes = [UInt8](data)
        try bytes.withUnsafeBufferPointer { buf in
            // the C API treats a NULL pointer as "no data"; give empty data a valid pointer
            var dummy: UInt8 = 0
            try withUnsafePointer(to: &dummy) { dummyPtr in
                o.value_data = buf.count > 0 ? buf.baseAddress : dummyPtr
                o.value_length = buf.count
                try perform(s) { chdman_add_metadata(ctx, &o) }
            }
        }
    }

    /// Adds the contents of a file as metadata.
    public func addMetadata(input: String, tag: String, contentsOf file: String, index: UInt32 = 0, noChecksum: Bool = false, inputParent: String? = nil) throws {
        let s = CStrings()
        var o = metadataOptions(input, tag, index, noChecksum, inputParent, s)
        o.value_file = s[file]
        try perform(s) { chdman_add_metadata(ctx, &o) }
    }

    public func deleteMetadata(input: String, tag: String, index: UInt32 = 0, inputParent: String? = nil) throws {
        let s = CStrings()
        try perform(s) { chdman_del_metadata(ctx, s[input], s[inputParent], s[tag], index) }
    }

    public func metadata(input: String, tag: String, index: UInt32 = 0, inputParent: String? = nil) throws -> Data {
        let s = CStrings()
        var data: UnsafeMutablePointer<UInt8>?
        var length = 0
        defer { if let data { chdman_free(data) } }
        try perform(s) { chdman_get_metadata(ctx, s[input], s[inputParent], s[tag], index, &data, &length) }
        return data.map { Data(bytes: $0, count: length) } ?? Data()
    }

    /// Writes one metadata item to a file.
    public func dumpMetadata(input: String, tag: String, output: String, index: UInt32 = 0, force: Bool = false, inputParent: String? = nil) throws {
        let s = CStrings()
        try perform(s) { chdman_dump_metadata(ctx, s[input], s[inputParent], s[tag], index, s[output], force ? 1 : 0) }
    }

    // MARK: internals

    private func perform(_ strings: CStrings, _ call: () -> chdman_status) throws {
        try lock.locked {
            if busy {
                throw ChdmanError(status: .invalidArgument, message: "This Chdman instance is already running an operation; use one instance per concurrent operation.")
            }
            if taskCancelled {
                throw CancellationError()
            }
            busy = true
        }
        defer { lock.locked { busy = false } }
        let status = withExtendedLifetime(strings) { call() }
        guard status != CHDMAN_OK else { return }
        if status == CHDMAN_ERR_CANCELLED && lock.locked({ taskCancelled }) {
            throw CancellationError()
        }
        var message = String(cString: chdman_last_error(ctx))
        if message.isEmpty { message = String(cString: chdman_status_string(status)) }
        throw ChdmanError(status: ChdmanError.Status(rawValue: Int(status.rawValue)) ?? .unknown, message: message)
    }

    private func progress(phase: Int, fraction: Double, ratio: Double) -> Bool {
        if lock.locked({ taskCancelled }) { return true }
        let cancel = onProgress?(ChdProgress(phase: ChdProgress.Phase(rawValue: phase) ?? .compressing, fraction: fraction, ratio: ratio)) ?? false
        return cancel || lock.locked { taskCancelled }
    }

    private func apply(_ out: inout chdman_output_options, _ output: String, _ settings: ChdOutputSettings, _ s: CStrings) {
        out.output = s[output]
        out.output_parent = s[settings.parent]
        out.force = settings.force ? 1 : 0
        out.hunk_size = settings.hunkSize
        out.compression = s[settings.compression]
    }

    private func apply(_ r: inout chdman_range_options, _ range: ChdRange?) {
        r.start_byte = range?.startByte ?? UInt64.max
        r.start_hunk = range?.startHunk ?? UInt64.max
        r.length_bytes = range?.lengthBytes ?? UInt64.max
        r.length_hunks = range?.lengthHunks ?? UInt64.max
    }

    private func metadataOptions(_ input: String, _ tag: String, _ index: UInt32, _ noChecksum: Bool, _ inputParent: String?, _ s: CStrings) -> chdman_add_metadata_options {
        var o = chdman_add_metadata_options()
        chdman_add_metadata_options_init(&o)
        o.input = s[input]
        o.input_parent = s[inputParent]
        o.tag = s[tag]
        o.index = index
        o.no_checksum = noChecksum ? 1 : 0
        return o
    }
}

/// C copies of Swift strings that live until the pool is released.
private final class CStrings {
    private var pointers: [UnsafeMutablePointer<CChar>] = []
    subscript(_ string: String?) -> UnsafePointer<CChar>? {
        guard let string, let p = strdup(string) else { return nil }
        pointers.append(p)
        return UnsafePointer(p)
    }
    deinit { pointers.forEach { free($0) } }
}

/// A NUL-terminated char[N] field imported as a tuple.
private func fixedString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
}

private extension NSLock {
    // NSLocking.withLock needs macOS 14; lib-chd-kit supports macOS 11
    func locked<R>(_ body: () throws -> R) rethrows -> R {
        lock()
        defer { unlock() }
        return try body()
    }
}
