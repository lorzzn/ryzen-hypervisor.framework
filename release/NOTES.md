Based on upstream 0.7.9-beta, including the Android PKU/OSPKE compatibility fix, read-only memory mapping fallback and Docker settings-file creation fix.

This fork also includes driver-owned native initialization, safe HyperKit stale-PID recovery with a lifetime lock, and Docker host-directory sharing through the fileserver FD handoff service.

The complete macOS x86_64 tar.gz bundle follows the upstream layout. GitHub CI compiles the kext, helper tools and compatibility libraries from this tag's source. QEMU is the hash-verified external dependency from upstream's 0.7.9-beta release.

Install with `./rhv kext --native` and reboot. This fork's native installer adds both `rhv_hv=1` and `rhv_hv_sysctl=1` so the driver initializes virtualization support at boot.

Host regression tests, linked and mapped compatibility-payload tests, native installer tests and both Parallels library re-export tests run before packaging. Archive layout, checksum, source revision, kmod version, plist version and driver banner are checked before publication.

This 0.7.9-based revision is a prerelease awaiting a hardware boot. Earlier hardware verification covered the 0.7.8-based compatibility revision. Hardware testing of the new kext, sleep/wake and other hypervisors remains pending.
