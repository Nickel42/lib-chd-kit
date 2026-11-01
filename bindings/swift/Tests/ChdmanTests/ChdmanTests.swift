// Tests for the Swift binding.
//   scripts/make-xcframework.sh && LIBCHDKIT_LOCAL_XCFRAMEWORK=1 swift test
// SPDX-License-Identifier: BSD-3-Clause

import Chdman
import CryptoKit
import Foundation
import XCTest

final class ChdmanTests: XCTestCase {
    var dir: URL!

    override func setUpWithError() throws {
        dir = FileManager.default.temporaryDirectory.appendingPathComponent("lib-chd-kit-swift-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: dir)
    }

    func path(_ name: String) -> String { dir.appendingPathComponent(name).path }

    /// noise, ramp, zeros, then a repeat of the noise (dedupes)
    func makeImage() throws -> (path: String, sha1: String, bytes: [UInt8]) {
        var bytes = [UInt8](repeating: 0, count: 4 * 65536 + 512 * 100)
        var seed: UInt32 = 1234
        for i in 0..<65536 {
            seed = seed &* 1664525 &+ 1013904223
            bytes[i] = UInt8(truncatingIfNeeded: seed >> 24)
        }
        for i in 65536..<(2 * 65536) { bytes[i] = UInt8(truncatingIfNeeded: i / 7) }
        bytes.replaceSubrange((3 * 65536)..<(4 * 65536), with: bytes[0..<65536])
        let p = path("raw.img")
        try Data(bytes).write(to: URL(fileURLWithPath: p))
        let sha1 = Insecure.SHA1.hash(data: Data(bytes)).map { String(format: "%02x", $0) }.joined()
        return (p, sha1, bytes)
    }

    func testVersionAndTemplates() {
        XCTAssertTrue(Chdman.version.contains("0.289"))
        XCTAssertEqual(Chdman.abiVersion, 1)
        let templates = Chdman.templates
        XCTAssertEqual(templates.count, 17)
        XCTAssertEqual(templates[0].manufacturer, "Conner")
        XCTAssertEqual(templates[0].cylinders, 332)
    }

    func testRawRoundTrip() throws {
        let image = try makeImage()
        let chdman = try Chdman()
        var reports: [ChdProgress] = []
        var logs: [(ChdLogLevel, String)] = []
        chdman.onProgress = { reports.append($0); return false }
        chdman.onLog = { logs.append(($0, $1)) }

        try chdman.createRaw(input: image.path, output: path("raw.chd"), unitSize: 512)
        XCTAssertTrue(reports.contains { $0.phase == .compressing && $0.fraction == 1.0 })
        XCTAssertTrue(logs.contains { $0.1.contains("Compression complete") })

        let info = try chdman.info(path("raw.chd"), hunkStats: true)
        XCTAssertEqual(info.version, 5)
        XCTAssertEqual(info.logicalBytes, UInt64(image.bytes.count))
        XCTAssertEqual(info.hunkBytes, 4096)
        XCTAssertEqual(info.codecs, ["lzma", "zlib", "huff", "flac"])
        XCTAssertEqual(info.mediaType, "raw")
        XCTAssertEqual(info.rawSHA1, image.sha1)
        XCTAssertEqual(info.hunkStats.reduce(0) { $0 + $1.hunks }, info.hunkCount)
        XCTAssertTrue(info.hunkStats.contains { $0.name == "Copy from self" })

        let verify = try chdman.verify(path("raw.chd"))
        XCTAssertEqual(verify.outcome, .ok)
        XCTAssertEqual(verify.computedRawSHA1, image.sha1)

        try chdman.extractRaw(input: path("raw.chd"), output: path("raw.out"))
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: path("raw.out"))), Data(image.bytes))

        try chdman.extractRaw(input: path("raw.chd"), output: path("part.out"), range: ChdRange(startHunk: 1, lengthBytes: 1000))
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: path("part.out"))), Data(image.bytes[4096..<5096]))

        try chdman.copy(input: path("raw.chd"), output: path("copy.chd"), settings: ChdOutputSettings(compression: "zstd", hunkSize: 8192))
        let copy = try chdman.info(path("copy.chd"))
        XCTAssertEqual(copy.codecs, ["zstd"])
        XCTAssertEqual(copy.hunkBytes, 8192)
        XCTAssertEqual(copy.rawSHA1, image.sha1)
    }

    func testErrors() throws {
        let image = try makeImage()
        let chdman = try Chdman()
        try chdman.createRaw(input: image.path, output: path("raw.chd"), unitSize: 512)
        XCTAssertThrowsError(try chdman.createRaw(input: image.path, output: path("raw.chd"), unitSize: 512)) {
            XCTAssertEqual(($0 as? ChdmanError)?.status, .fileExists)
        }
        XCTAssertThrowsError(try chdman.createRaw(input: image.path, output: path("bad.chd"), unitSize: 512, settings: ChdOutputSettings(compression: "abcd"))) {
            XCTAssertEqual(($0 as? ChdmanError)?.status, .invalidArgument)
        }

        // a .cue naming a missing .bin reports the cause in the error and the log
        try "FILE \"nothere.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
            .write(toFile: path("missing.cue"), atomically: true, encoding: .utf8)
        var errors: [String] = []
        chdman.onLog = { level, message in if level == .error { errors.append(message) } }
        XCTAssertThrowsError(try chdman.createCD(input: path("missing.cue"), output: path("missing.chd"))) {
            XCTAssertTrue(($0 as? ChdmanError)?.message.contains("nothere.bin") == true, "\($0)")
        }
        XCTAssertTrue(errors.contains { $0.contains("nothere.bin") })
    }

    func testMetadataAndHardDisk() throws {
        let image = try makeImage()
        let chdman = try Chdman()
        try chdman.createRaw(input: image.path, output: path("plain.chd"), unitSize: 512, settings: ChdOutputSettings(compression: "none"))
        try chdman.addMetadata(input: path("plain.chd"), tag: "TEST", text: "hello wörld")
        try chdman.addMetadata(input: path("plain.chd"), tag: "BIN", data: Data([1, 2, 3]), noChecksum: true)
        XCTAssertEqual(try chdman.metadata(input: path("plain.chd"), tag: "BIN"), Data([1, 2, 3]))
        let info = try chdman.info(path("plain.chd"))
        XCTAssertEqual(info.metadata.count, 2)
        XCTAssertEqual(info.metadata[0].tagString, "TEST")
        XCTAssertEqual(info.metadata[0].text, "hello wörld")
        XCTAssertEqual(info.metadata[1].tagString, "BIN ")
        XCTAssertFalse(info.metadata[1].checksummed)
        try chdman.deleteMetadata(input: path("plain.chd"), tag: "TEST")
        XCTAssertThrowsError(try chdman.metadata(input: path("plain.chd"), tag: "TEST")) {
            XCTAssertEqual(($0 as? ChdmanError)?.status, .notFound)
        }

        try chdman.createHardDisk(output: path("blank.chd"), disk: ChdHardDiskOptions(templateID: 1))
        let blank = try chdman.info(path("blank.chd"))
        XCTAssertEqual(blank.mediaType, "hd")
        XCTAssertEqual(blank.logicalBytes, 321 * 2 * 16 * 512)
        XCTAssertFalse(blank.isCompressed)
    }

    func testCancelFromCallbackRemovesOutput() throws {
        let image = try makeImage()
        let chdman = try Chdman()
        chdman.onProgress = { _ in true }
        XCTAssertThrowsError(try chdman.createRaw(input: image.path, output: path("cancel.chd"), unitSize: 512)) {
            XCTAssertEqual(($0 as? ChdmanError)?.status, .cancelled)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: path("cancel.chd")))
        chdman.onProgress = nil
        try chdman.createRaw(input: image.path, output: path("after.chd"), unitSize: 512)
    }

    func testAsyncRunAndTaskCancellation() async throws {
        let image = try makeImage()
        let chdman = try Chdman()
        let out = path("async.chd")
        try await chdman.run { try $0.createRaw(input: image.path, output: out, unitSize: 512) }
        let sha = try await chdman.run { try $0.info(out).rawSHA1 }
        XCTAssertEqual(sha, image.sha1)

        // cancelling the task cancels the operation and removes the output
        // the first progress report waits until the task has been cancelled
        let started = expectation(description: "progress")
        started.assertForOverFulfill = false
        let resume = DispatchSemaphore(value: 0)
        chdman.onProgress = { _ in started.fulfill(); _ = resume.wait(timeout: .now() + 10); return false }
        let cancelled = path("cancelled.chd")
        let task = Task { try await chdman.run { try $0.createRaw(input: image.path, output: cancelled, unitSize: 512) } }
        await fulfillment(of: [started], timeout: 10)
        task.cancel()
        resume.signal()
        do {
            try await task.value
            XCTFail("expected cancellation")
        } catch is CancellationError {
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: cancelled))

        // a cancel that arrived after the run finished doesn't leak into the next operation
        chdman.onProgress = nil
        try chdman.createRaw(input: image.path, output: path("next.chd"), unitSize: 512)
    }
}
