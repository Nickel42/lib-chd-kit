// Example: create, inspect, verify and extract a raw CHD from Swift.
//
// Apps should depend on the Swift package (see bindings/swift/README.md).
// To try this file against a local build without the package:
//   swiftc -I ../../include -L <build> -lchdkit -lc++ -Xlinker -rpath -Xlinker <build> \
//     ../../bindings/swift/Sources/Chdman/Chdman.swift main.swift -o chdman-swift
// SPDX-License-Identifier: BSD-3-Clause

import Foundation

let dir = FileManager.default.temporaryDirectory.appendingPathComponent("chdman-swift-example")
try? FileManager.default.removeItem(at: dir)
try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
let image = dir.appendingPathComponent("disk.img").path
let chd = dir.appendingPathComponent("disk.chd").path
let back = dir.appendingPathComponent("disk.out").path

// 1 MiB test image: half pattern, half zeroes
var bytes = [UInt8](repeating: 0, count: 1 << 20)
for i in 0..<(bytes.count / 2) { bytes[i] = UInt8(truncatingIfNeeded: i * 7) }
try Data(bytes).write(to: URL(fileURLWithPath: image))

print("lib-chd-kit \(Chdman.version)")
let chdman = try Chdman()
chdman.onProgress = { progress in
    if progress.phase == .compressing { print(String(format: "  compressing %.0f%%", progress.fraction * 100)) }
    return false // return true to cancel
}

try chdman.createRaw(input: image, output: chd, unitSize: 512, settings: ChdOutputSettings(force: true))
let info = try chdman.info(chd)
print("created \(info.mediaType) CHD v\(info.version): \(info.logicalBytes) -> \(info.fileBytes) bytes, \(info.compression)")
print("raw SHA-1 \(info.rawSHA1)")
print("verify: \(try chdman.verify(chd).outcome)")
try chdman.extractRaw(input: chd, output: back, force: true)
print("round trip identical: \(try Data(contentsOf: URL(fileURLWithPath: back)) == Data(bytes))")

do {
    try chdman.createRaw(input: image, output: chd, unitSize: 512) // exists, no force
} catch let error as ChdmanError {
    print("expected error -> \(error.status): \(error)")
}
