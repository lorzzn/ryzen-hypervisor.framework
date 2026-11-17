"""Publish a validated CI artifact, keeping incomplete uploads in a draft."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


here = Path(__file__).resolve().parent
version = (here / "VERSION").read_text().strip()
tag = "v" + version
repository = os.environ["GITHUB_REPOSITORY"]
source = os.environ["GITHUB_SHA"]
if os.environ.get("GITHUB_ACTIONS") != "true" or os.environ["GITHUB_REF_NAME"] != tag:
    raise SystemExit("Publication requires the matching GitHub Actions tag run")

archive = Path("dist") / f"ryzenhv-{version}-macos-x86_64.tar.gz"
files = [archive, archive.with_name(archive.name + ".sha256")]
subprocess.run([
    sys.executable, str(here / "check-package.py"), str(archive),
    "--version", version, "--source", source,
], check=True)


def api(path, payload=None):
    arguments = ["gh", "api", path]
    data = None
    if payload is not None:
        arguments.extend(["--method", "PATCH", "--input", "-"])
        data = json.dumps(payload).encode()
    return json.loads(subprocess.check_output(arguments, input=data))


environment = dict(os.environ, RHV_RELEASE_TAG=tag)
existing = subprocess.check_output([
    "gh", "api", f"repos/{repository}/releases", "--paginate",
    "--jq", ".[] | select(.tag_name == env.RHV_RELEASE_TAG)",
], env=environment).strip()
release = json.loads(existing) if existing else None
if release and release["target_commitish"] != source:
    raise SystemExit("The existing release points to another commit")
if release and not release["draft"]:
    expected_names = {path.name for path in files}
    if {asset["name"] for asset in release["assets"]} != expected_names:
        raise SystemExit("The published release is incomplete")
    print(f"Already published {tag} from {source}")
    raise SystemExit(0)

if release:
    subprocess.run([
        "gh", "release", "upload", tag, *(str(path) for path in files),
        "--repo", repository, "--clobber",
    ], check=True)
else:
    notes = Path(os.environ["RUNNER_TEMP"]) / "ryzenhv-release-notes.md"
    run_url = f"https://github.com/{repository}/actions/runs/{os.environ['GITHUB_RUN_ID']}"
    notes.write_text(
        (here / "NOTES.md").read_text()
        + f"\nBuilt and published by GitHub Actions. [Build run]({run_url}).\n"
        + f"\nSource commit `{source}`.\n"
    )
    subprocess.run([
        "gh", "release", "create", tag, *(str(path) for path in files),
        "--repo", repository, "--verify-tag", "--target", source,
        "--title", f"{version} beta", "--notes-file", str(notes),
        "--draft", "--prerelease",
    ], check=True)
    existing = subprocess.check_output([
        "gh", "api", f"repos/{repository}/releases", "--paginate",
        "--jq", ".[] | select(.tag_name == env.RHV_RELEASE_TAG)",
    ], env=environment)
    release = json.loads(existing)

release = api(f"repos/{repository}/releases/{release['id']}")
assets = {asset["name"]: asset for asset in release["assets"]}
assert set(assets) == {path.name for path in files}
for path in files:
    asset = assets[path.name]
    assert asset["state"] == "uploaded" and asset["size"] == path.stat().st_size
    assert asset["digest"] == "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
published = api(f"repos/{repository}/releases/{release['id']}", {"draft": False})
assert not published["draft"] and published["published_at"]
print(published["html_url"])
