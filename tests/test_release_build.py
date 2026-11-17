import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin", "Requires the macOS linker and plist tools")
class ReleaseBuildTests(unittest.TestCase):
    def test_parallels_wrappers_reexport_the_runtime_library(self):
        for target in ["hijack", "hijack-patchonly"]:
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                folder = Path(directory)
                build = subprocess.run(
                    ["make", target, "HIJACK_ORIG=release/libPrlXmlModel.1.tbd"],
                    cwd=ROOT, capture_output=True, text=True,
                )
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                original = folder / "libPrlXmlModel.1.orig.dylib"
                vendor = folder / "vendor.c"
                vendor.write_text("int release_vendor_value(void) { return 73; }\n")
                subprocess.run([
                    "clang", "-arch", "x86_64", "-dynamiclib", str(vendor),
                    "-install_name", "@rpath/libPrlXmlModel.1.dylib", "-o", str(original),
                ], check=True, capture_output=True)
                client = folder / "client.c"
                client.write_text(
                    "extern int release_vendor_value(void);\n"
                    "int main(void) { return release_vendor_value() == 73 ? 0 : 1; }\n"
                )
                executable = folder / "reexport-check"
                subprocess.run([
                    "clang", "-arch", "x86_64", str(client), str(original),
                    "-Wl,-rpath,@executable_path", "-o", str(executable),
                ], check=True, capture_output=True)
                subprocess.run([
                    "install_name_tool", "-id",
                    "@loader_path/libPrlXmlModel.1.orig.dylib", str(original),
                ], check=True, capture_output=True)
                wrapper = folder / "libPrlXmlModel.1.dylib"
                shutil.copy2(ROOT / "build/libPrlXmlModel.1.dylib", wrapper)
                for library in [original, wrapper]:
                    subprocess.run(
                        ["codesign", "--force", "--sign", "-", str(library)],
                        check=True, capture_output=True,
                    )
                result = subprocess.run([str(executable)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_installer_enables_driver_initialization_once(self):
        with tempfile.TemporaryDirectory(prefix="RyzenHV installer ") as directory:
            folder = Path(directory)
            shutil.copy2(ROOT / "release/rhv", folder / "rhv")
            (folder / "VERSION").write_text("version=0.7.9-compat.1\nabi=11\n")
            (folder / "RyzenHV.kext/Contents").mkdir(parents=True)
            efi = folder / "Test EFI"
            oc = efi / "EFI/OC"
            (oc / "Kexts").mkdir(parents=True)
            guid = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
            config = {
                "Kernel": {"Add": []},
                "NVRAM": {
                    "Add": {guid: {"boot-args": "-v existing=1"}},
                    "Delete": {guid: ["boot-args"]},
                },
            }
            path = oc / "config.plist"
            path.write_bytes(plistlib.dumps(config))
            environment = dict(os.environ, RHV_YES="1")
            for _ in range(2):
                result = subprocess.run(
                    ["/bin/bash", str(folder / "rhv"), "kext", "--native", str(efi)],
                    env=environment, capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            installed = plistlib.loads(path.read_bytes())
            arguments = installed["NVRAM"]["Add"][guid]["boot-args"].split()
            self.assertIn("existing=1", arguments)
            self.assertEqual(arguments.count("rhv_hv=1"), 1)
            self.assertEqual(arguments.count("rhv_hv_sysctl=1"), 1)
            self.assertEqual(len(installed["Kernel"]["Add"]), 1)


if __name__ == "__main__":
    unittest.main()
