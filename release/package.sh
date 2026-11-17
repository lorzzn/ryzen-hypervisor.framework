#!/bin/bash
#
# Builds the release bundle: dist/ryzenhv-<version>-macos-x86_64.tar.gz
#
#   release/package.sh <version> <qemu-dir>
#
# <qemu-dir> is a directory with the already built, relocatable QEMU (bin/qemu-system-x86_64, lib/*.dylib, share/qemu/*),
# made by release/bundle-qemu.sh. Everything else is built here from the sources, so the kext, the shim and the
# stand-in library of one bundle always come from one build (the ABI numbers match).
# Parallels' original library is represented by a link-time stub and supplied by the installed app at runtime.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
VER="${1:?usage: package.sh <version> <qemu-dir>}"
QEMU_DIR="${2:?usage: package.sh <version> <qemu-dir>}"
cd "$HERE"
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9][A-Za-z0-9.-]*)?$ ]] || {
    echo "invalid release version: $VER" >&2
    exit 2
}
SHORT_VERSION="$(plutil -extract CFBundleShortVersionString raw -o - kext/Info.plist)"
[ "${VER%%-*}" = "$SHORT_VERSION" ] || {
    echo "release version $VER does not match kext version $SHORT_VERSION" >&2
    exit 2
}

ABI="$(sed -n 's/^#define RHV_ABI_VERSION[[:space:]]*\([0-9]*\).*/\1/p' include/RyzenHVShared.h)"
[ -n "$ABI" ] || { echo "cannot read RHV_ABI_VERSION" >&2; exit 1; }

make -C kext clean >/dev/null
rm -f build/libryzenhv.dylib build/libPrlXmlModel.1.dylib
make kext shim tools >/dev/null
make hijack HIJACK_ORIG="$HERE/release/libPrlXmlModel.1.tbd" >/dev/null
cp build/libPrlXmlModel.1.dylib build/prl-shim.dylib
make hijack-patchonly HIJACK_ORIG="$HERE/release/libPrlXmlModel.1.tbd" >/dev/null
cp build/libPrlXmlModel.1.dylib build/prl-native.dylib

S="dist/ryzenhv-$VER"
rm -rf "$S" "dist/ryzenhv-$VER-macos-x86_64.tar.gz"
mkdir -p "$S/bin" "$S/lib" "$S/libexec" "$S/docs" "$S/LICENSES"
cp release/rhv "$S/rhv"
cp release/libexec/serial-drive.pl "$S/libexec/"
cp scripts/parallels-shim.sh scripts/docker-desktop-shim.sh scripts/hv-sysctl-daemon.sh "$S/libexec/"
cp -R build/RyzenHV.kext "$S/RyzenHV.kext"
cp build/libryzenhv.dylib "$S/lib/"
cp build/prl-shim.dylib "$S/lib/libPrlXmlModel.1.dylib"
cp build/prl-native.dylib "$S/lib/libPrlXmlModel.1.native.dylib"
cp build/rhv-info build/rhv-patchcheck "$S/bin/"
cp -R "$QEMU_DIR" "$S/qemu"
cp README.md "$S/"
cp docs/PARALLELS.md docs/DOCKER.md docs/DOCKER-DESKTOP.md docs/NATIVE.md docs/RYZENTOSH.md "$S/docs/"
cp release/README-BUNDLE.md "$S/START-HERE.md"
cp release/LICENSES/* "$S/LICENSES/"
cp LICENSE "$S/LICENSES/RyzenHV-LICENSE" 2>/dev/null || true
printf 'version=%s\nabi=%s\nparallels_build=57613\nbuilt=%s\nsource=%s\nci_run=%s\nqemu_source=%s\nqemu_sha256=%s\n' \
    "$VER" "$ABI" "$(date +%s)" "$(git rev-parse HEAD 2>/dev/null || echo unknown)" \
    "${GITHUB_RUN_ID:-local}" "${QEMU_SOURCE:-local}" "${QEMU_SHA256:-unknown}" > "$S/VERSION"
find "$S" -name '.DS_Store' -delete
find "$S" -name '*.dSYM' -prune -exec rm -rf {} \;

# Nothing in the bundle may carry the build machine's user name.
U="$(id -un)"
if LC_ALL=C grep -rl "$U" "$S" 2>/dev/null | grep -q .; then
    echo "WARNING: the user name '$U' is inside:" >&2
    LC_ALL=C grep -rl "$U" "$S" >&2
fi

tar -C dist -czf "dist/ryzenhv-$VER-macos-x86_64.tar.gz" "ryzenhv-$VER"
( cd dist && shasum -a 256 "ryzenhv-$VER-macos-x86_64.tar.gz" > "ryzenhv-$VER-macos-x86_64.tar.gz.sha256" )
ls -la dist/ryzenhv-$VER-macos-x86_64.tar.gz*
