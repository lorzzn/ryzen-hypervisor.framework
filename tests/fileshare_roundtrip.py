"""Explicit integration probe for Docker's FD fileserver. Installs no service."""

import argparse
import array
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import uuid


DATA = Path.home() / "Library/Containers/com.docker.docker/Data"
ENTRY = DATA / "vms/0/00000002.00001003"
DOCKER = "/Applications/Docker.app/Contents/Resources/bin/docker"
stop = threading.Event()
errors = []
handoffs = []


def bridge():
    listener = None
    identity = None
    try:
        while not stop.is_set():
            if not ENTRY.parent.is_dir():
                stop.wait(0.1)
                continue
            if not ENTRY.exists():
                if listener:
                    listener.close()
                listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                listener.bind(str(ENTRY))
                ENTRY.chmod(0o600)
                listener.listen(8)
                listener.settimeout(0.4)
                status = ENTRY.stat()
                identity = (status.st_dev, status.st_ino)
            elif identity != (ENTRY.stat().st_dev, ENTRY.stat().st_ino):
                raise RuntimeError("test entry was replaced by another owner")
            try:
                client, _ = listener.accept()
            except socket.timeout:
                continue
            with client, socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as control:
                control.settimeout(3)
                control.connect(str(DATA / "filesystem-fd.sock"))
                control.sendmsg(
                    [b"\x00"],
                    [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [client.fileno()]))],
                )
                if control.recv(1) != b"\xfd":
                    raise RuntimeError("bad fileserver acknowledgement")
                handoffs.append(time.monotonic())
    except Exception as error:
        errors.append(repr(error))
    finally:
        if listener:
            listener.close()
        if identity and ENTRY.exists():
            status = ENTRY.lstat()
            if (status.st_dev, status.st_ino) == identity:
                ENTRY.unlink()


def docker(arguments, timeout=18):
    result = subprocess.run(
        [DOCKER, *arguments], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=timeout, text=True,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    return result.stdout


parser = argparse.ArgumentParser()
parser.add_argument("--restart", action="store_true")
parser.add_argument("--wait-engine", action="store_true")
arguments = parser.parse_args()
if ENTRY.exists() or ENTRY.is_symlink():
    raise SystemExit("The legacy socket entry already exists. Probe did not change it.")
if arguments.restart and docker(["ps", "-aq"]).strip():
    raise SystemExit("Containers exist. Probe did not restart Docker.")

report = {"handoffs": 0, "checks": {}}
name = "rhv-roundtrip-" + uuid.uuid4().hex[:10]
thread = threading.Thread(target=bridge, daemon=True)
thread.start()
temporary = tempfile.TemporaryDirectory(
    prefix="共享 目录 ", dir=Path(__file__).resolve().parents[1] / "build"
)
folder = Path(temporary.name)
try:
    deadline = time.monotonic() + 3
    while not ENTRY.exists() and time.monotonic() < deadline and not errors:
        time.sleep(0.05)
    if arguments.restart or arguments.wait_engine:
        if arguments.restart:
            try:
                docker(["desktop", "restart", "--detach"], timeout=30)
            except subprocess.TimeoutExpired:
                report["restart_command_timed_out"] = True
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            try:
                if docker(["info", "--format", "{{.ServerVersion}}"], timeout=4).strip():
                    break
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
            time.sleep(1)
        else:
            raise RuntimeError("Docker did not become ready after restart")
    (folder / "from-host").write_text("host 你好")
    output = docker([
        "run", "-d", "--pull=never", "--name", name, "--mount",
        f"type=bind,src={folder},dst=/check", "busybox:1.37.0", "sleep", "300",
    ])
    report["container"] = output.strip()
    report["checks"]["host_to_guest"] = docker(
        ["exec", name, "cat", "/check/from-host"]
    ) == "host 你好"
    docker(["exec", name, "sh", "-c", "printf 'guest hello' > /check/from-guest"])
    report["checks"]["guest_to_host"] = (folder / "from-guest").read_text() == "guest hello"
    (folder / "replacement").write_text("replacement 你好")
    (folder / "replacement").replace(folder / "from-host")
    report["checks"]["atomic_replacement"] = docker(
        ["exec", name, "cat", "/check/from-host"]
    ) == "replacement 你好"
    docker(["rm", "-f", name])
    docker([
        "run", "--rm", "--pull=never", "--name", name, "--mount",
        f"type=bind,src={folder},dst=/check,readonly", "busybox:1.37.0",
        "sh", "-c", "if echo forbidden > /check/forbidden; then exit 7; fi",
    ])
    report["checks"]["readonly"] = not (folder / "forbidden").exists()
except Exception as error:
    report["failure"] = repr(error)
finally:
    try:
        docker(["rm", "-f", name], timeout=5)
    except Exception:
        pass
    stop.set()
    thread.join(timeout=5)
    temporary.cleanup()
    report["handoffs"] = len(handoffs)
    report["bridge_errors"] = errors
    report["entry_removed"] = not ENTRY.exists() and not ENTRY.is_symlink()
    path = Path(__file__).resolve().parents[1] / "build/fileshare-roundtrip.json"
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))
if report.get("failure") or errors or not all(report["checks"].values()) or not report["checks"]:
    raise SystemExit(1)
