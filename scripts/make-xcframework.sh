#!/bin/sh
# Builds build-xcframework/CChdman.xcframework (universal arm64 + x86_64
# static libchdkit with the CChdman module) for the Swift package.
#
#   scripts/make-xcframework.sh                    local build; then
#                                                  LIBCHDKIT_LOCAL_XCFRAMEWORK=1 swift test
#   scripts/make-xcframework.sh --release 1.0.0    also zips it and writes the
#                                                  version and checksum into Package.swift
#
# Requires macOS with Xcode command line tools and CMake 3.20+.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
release=""
if [ "${1:-}" = "--release" ]; then
	release=${2:?usage: make-xcframework.sh --release <version>}
fi

out="$root/build-xcframework"
work="$out/cmake"

cmake -S "$root" -B "$work" -DCMAKE_BUILD_TYPE=Release \
	"-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" \
	-DCHDMAN_BUILD_SHARED=OFF -DCHDMAN_BUILD_CLI=OFF -DCHDMAN_BUILD_TESTS=OFF
cmake --build "$work" --parallel

# The package links libc++ itself (Package.swift), and SwiftPM links the
# archive directly, so this module map has no link directives.
headers="$out/headers"
rm -rf "$headers"
mkdir -p "$headers"
cp "$root/include/chdman.h" "$headers/"
cat > "$headers/module.modulemap" <<'EOF'
module CChdman {
    header "chdman.h"
    export *
}
EOF

rm -rf "$out/CChdman.xcframework"
xcodebuild -create-xcframework -library "$work/libchdkit.a" -headers "$headers" -output "$out/CChdman.xcframework"
echo "built $out/CChdman.xcframework"

if [ -n "$release" ]; then
	zip="$out/CChdman.xcframework.zip"
	rm -f "$zip"
	(cd "$out" && ditto -c -k --sequesterRsrc --keepParent CChdman.xcframework CChdman.xcframework.zip)
	checksum=$(swift package compute-checksum "$zip")
	sed -i '' \
		-e "s/^let releaseVersion = .*/let releaseVersion = \"$release\"/" \
		-e "s/^let releaseChecksum = .*/let releaseChecksum = \"$checksum\"/" \
		"$root/Package.swift"
	echo "wrote version $release and checksum $checksum to Package.swift"
	echo "next: commit Package.swift, tag v$release, and attach $zip to the v$release GitHub release"
fi
