import array
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = Path(os.environ.get("RHV_COMPAT_RUNNER", ROOT / "build/hyperkit_compat_test"))


class HyperKitCompatTests(unittest.TestCase):
    def setUp(self):
        # Darwin Unix socket names are limited to 104 bytes.
        self.temporary = tempfile.TemporaryDirectory(prefix="rhv-", dir="/tmp")
        self.root = Path(self.temporary.name)
        self.vm = self.root / "vms" / "0"
        self.vm.mkdir(parents=True)
        self.pid = self.vm / "hyperkit.pid"
        self.children = []
        self.server_errors = []
        self.server_threads = []

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=4)
        for thread in self.server_threads:
            thread.join(timeout=5)
            self.assertFalse(thread.is_alive(), "fileserver fixture did not finish")
        self.temporary.cleanup()
        self.assertEqual(self.server_errors, [])

    def run_pid(self, **environment):
        return subprocess.run(
            [str(RUNNER), "pid", str(self.pid)],
            env=dict(os.environ, **environment),
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
        )

    def test_new_pid_file(self):
        result = self.run_pid()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.pid.read_text(), result.stdout.split()[1])

    def test_stale_pid_file(self):
        self.pid.write_text("999999999")
        result = self.run_pid()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.pid.read_text(), result.stdout.split()[1])

    def test_corrupt_pid_file(self):
        self.pid.write_text("previous failed write\n")
        result = self.run_pid()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_pid_reused_by_unrelated_process(self):
        self.pid.write_text(str(os.getpid()))
        result = self.run_pid()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_existing_live_vm_is_preserved(self):
        original = str(os.getpid())
        self.pid.write_text(original)
        result = self.run_pid(
            RHV_TEST_ACTIVE_PID=original, RHV_TEST_ACTIVE_PATH=str(self.pid)
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.pid.read_text(), original)

    def test_existing_live_vm_path_alias_is_preserved(self):
        original = str(os.getpid())
        self.pid.write_text(original)
        result = self.run_pid(
            RHV_TEST_ACTIVE_PID=original,
            RHV_TEST_ACTIVE_PATH=str(self.vm / ".." / "0" / "hyperkit.pid"),
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.pid.read_text(), original)

    def test_existing_live_vm_with_attached_pid_option_is_preserved(self):
        original = str(os.getpid())
        self.pid.write_text(original)
        result = self.run_pid(
            RHV_TEST_ACTIVE_PID=original, RHV_TEST_ACTIVE_PATH=str(self.pid),
            RHV_TEST_ATTACHED_F="1",
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.pid.read_text(), original)

    def test_other_live_vm_does_not_block_stale_pid_recovery(self):
        original = str(os.getpid())
        self.pid.write_text(original)
        result = self.run_pid(
            RHV_TEST_ACTIVE_PID=original,
            RHV_TEST_ACTIVE_PATH=str(self.vm.parent / "1" / "hyperkit.pid"),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_lifetime_lock_blocks_duplicates_and_recovers_after_kill(self):
        holder = subprocess.Popen(
            [str(RUNNER), "hold", str(self.pid)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        self.children.append(holder)
        self.assertTrue(holder.stdout.readline().startswith("OK "))
        self.assertNotEqual(self.run_pid().returncode, 0)
        holder.kill()
        holder.communicate(timeout=4)
        result = self.run_pid()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_pid_symlink_is_preserved(self):
        target = self.root / "important"
        target.write_text("untouched")
        self.pid.symlink_to(target)
        self.assertNotEqual(self.run_pid().returncode, 0)
        self.assertEqual(target.read_text(), "untouched")
        self.assertTrue(self.pid.is_symlink())

    def test_lock_symlink_is_preserved(self):
        target = self.root / "important"
        target.write_text("untouched")
        Path(str(self.pid) + ".rhv-lock").symlink_to(target)
        self.assertNotEqual(self.run_pid().returncode, 0)
        self.assertEqual(target.read_text(), "untouched")

    def test_missing_pid_api_fails_without_crashing(self):
        result = self.run_pid(RHV_TEST_MISSING_SYMBOL="flock")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertFalse(self.pid.exists())

    def start_server(self, acknowledge=b"\xfd", *, ordinary=False, silent=False):
        path = self.root / ("normal.sock" if ordinary else "filesystem-fd.sock")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(path))
        listener.listen(1)
        listener.settimeout(5)

        def serve():
            try:
                connection, _ = listener.accept()
                with connection:
                    if ordinary:
                        self.assertEqual(connection.recv(4), b"ping")
                        connection.sendall(b"pong")
                        return
                    message, ancillary, _, _ = connection.recvmsg(
                        1, socket.CMSG_SPACE(array.array("i").itemsize)
                    )
                    self.assertEqual(message, b"\x00")
                    self.assertEqual(len(ancillary), 1)
                    level, kind, data = ancillary[0]
                    self.assertEqual((level, kind), (socket.SOL_SOCKET, socket.SCM_RIGHTS))
                    descriptors = array.array("i")
                    descriptors.frombytes(data[:descriptors.itemsize])
                    with socket.socket(fileno=descriptors[0]) as stream:
                        if silent:
                            time.sleep(3.5)
                            return
                        if acknowledge:
                            connection.sendall(acknowledge)
                        if acknowledge == b"\xfd":
                            self.assertEqual(stream.recv(4), b"ping")
                            stream.sendall(b"pong")
            except (TimeoutError, socket.timeout):
                # Baseline never connects to the replacement service.
                pass
            except Exception as error:
                self.server_errors.append(repr(error))
            finally:
                listener.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        self.server_threads.append(thread)
        return path

    def connect(self, path, **environment):
        return subprocess.run(
            [str(RUNNER), "connect", str(path)],
            env=dict(os.environ, **environment),
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=6,
        )

    def test_fileserver_handoff(self):
        self.start_server()
        result = self.connect(self.vm / "00000002.00001003")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_fileserver_handoff_preserves_nonblocking_and_close_on_exec(self):
        self.start_server()
        result = self.connect(self.vm / "00000002.00001003", RHV_TEST_NONBLOCK="1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unrelated_socket_is_unchanged(self):
        path = self.start_server(ordinary=True)
        result = self.connect(path)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_bad_acknowledgement_is_rejected(self):
        self.start_server(acknowledge=b"\x01")
        self.assertNotEqual(self.connect(self.vm / "00000002.00001003").returncode, 0)

    def test_absent_acknowledgement_is_rejected(self):
        self.start_server(acknowledge=b"")
        self.assertNotEqual(self.connect(self.vm / "00000002.00001003").returncode, 0)

    def test_silent_fileserver_obeys_deadline(self):
        self.start_server(silent=True)
        started = time.monotonic()
        result = self.connect(self.vm / "00000002.00001003")
        elapsed = time.monotonic() - started
        self.assertEqual(result.returncode, 2)
        self.assertGreaterEqual(elapsed, 2.7)
        self.assertLess(elapsed, 4.5)


if __name__ == "__main__":
    unittest.main()
