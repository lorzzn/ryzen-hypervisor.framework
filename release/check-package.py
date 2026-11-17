"""Validate a release archive before uploading or publishing it."""

import argparse
import hashlib
from pathlib import Path
import plistlib
import re
import struct
import tarfile


parser = argparse.ArgumentParser()
parser.add_argument("archive", type=Path)
parser.add_argument("--version", required=True)
parser.add_argument("--source", required=True)
args = parser.parse_args()

match = re.fullmatch(r"(\d+\.\d+\.\d+)-compat\.(\d+)", args.version)
if not match:
    raise SystemExit("Expected a version such as 0.7.9-compat.1")
base, revision = match.groups()
bundle_version = f"{base}d{revision}"
root = "ryzenhv-" + args.version
digest = hashlib.sha256(args.archive.read_bytes()).hexdigest()
checksum = args.archive.with_name(args.archive.name + ".sha256")
if checksum.read_text().split() != [digest, args.archive.name]:
    raise SystemExit("Archive checksum does not match")

with tarfile.open(args.archive, "r:gz") as package:
    names = package.getnames()
    assert len(names) == len(set(names)), "Duplicate archive entries"
    assert {name.split("/")[0] for name in names} == {root}, "Wrong archive root"
    assert all(".." not in name.split("/") for name in names), "Unsafe archive path"

    def read(relative):
        return package.extractfile(root + "/" + relative).read()

    metadata = dict(line.split("=", 1) for line in read("VERSION").decode().splitlines())
    assert metadata["version"] == args.version
    assert metadata["source"] == args.source
    assert metadata["qemu_source"] == (
        "https://github.com/OverlordDev/ryzen-hypervisor.framework/releases/tag/v0.7.9-beta"
    )
    assert metadata["qemu_sha256"] == (
        "dfe7eca98f3229f062f2973a3f9717012665914e108ecba91f54cd82656b23d2"
    )
    info = plistlib.loads(read("RyzenHV.kext/Contents/Info.plist"))
    assert info["CFBundleShortVersionString"] == base
    assert info["CFBundleVersion"] == bundle_version
    binary = read("RyzenHV.kext/Contents/MacOS/RyzenHV")
    magic, cpu, _, file_type = struct.unpack_from("<IIII", binary)
    assert (magic, cpu, file_type) == (0xFEEDFACF, 0x01000007, 11)
    assert bundle_version.encode() in binary, "Kmod version differs from the plist"
    assert f"{base}-compat{revision}".encode() in binary, "Driver banner differs from the tag"
    for relative in [
        "rhv", "bin/rhv-info", "bin/rhv-patchcheck",
        "RyzenHV.kext/Contents/MacOS/RyzenHV",
        "lib/libryzenhv.dylib", "lib/libPrlXmlModel.1.dylib",
        "lib/libPrlXmlModel.1.native.dylib", "qemu/bin/qemu-system-x86_64",
    ]:
        assert package.getmember(root + "/" + relative).mode & 0o111, relative
    assert read("LICENSES/RyzenHV-LICENSE"), "Missing project license"

print(f"Verified {args.archive.name}, source {args.source}, kext {bundle_version}")
