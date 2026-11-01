// swift-tools-version:5.9
// Swift package for lib-chd-kit (macOS). The native library ships as a
// prebuilt XCFramework attached to each GitHub release; the Swift API is in
// bindings/swift. See bindings/swift/README.md.
//
// To develop against a local build instead of the release download, run
// scripts/make-xcframework.sh and then
//   LIBCHDKIT_LOCAL_XCFRAMEWORK=1 swift build

import PackageDescription
import Foundation

// Updated by scripts/make-xcframework.sh --release <version>.
let releaseVersion = "1.0.0"
let releaseChecksum = "0000000000000000000000000000000000000000000000000000000000000000"

let useLocal = ProcessInfo.processInfo.environment["LIBCHDKIT_LOCAL_XCFRAMEWORK"] != nil

let native: Target = useLocal
    ? .binaryTarget(name: "CChdman", path: "build-xcframework/CChdman.xcframework")
    : .binaryTarget(
        name: "CChdman",
        url: "https://github.com/Nickel42/lib-chd-kit/releases/download/v\(releaseVersion)/CChdman.xcframework.zip",
        checksum: releaseChecksum)

let package = Package(
    name: "Chdman",
    platforms: [.macOS(.v11)],
    products: [
        .library(name: "Chdman", targets: ["Chdman"]),
    ],
    targets: [
        native,
        .target(
            name: "Chdman",
            dependencies: ["CChdman"],
            path: "bindings/swift/Sources/Chdman",
            linkerSettings: [.linkedLibrary("c++")]),
        .testTarget(
            name: "ChdmanTests",
            dependencies: ["Chdman"],
            path: "bindings/swift/Tests/ChdmanTests"),
    ]
)
