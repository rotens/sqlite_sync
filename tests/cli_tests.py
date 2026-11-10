"""Optional CLI integration checks, using only the Python standard library."""
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

executable = str(Path(sys.argv[1]).resolve())


def run(cwd, *args, code=0, contains=""):
    result = subprocess.run([executable, *args], cwd=cwd, capture_output=True, text=True)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    assert contains in result.stdout + result.stderr, (args, result.stdout, result.stderr)
    return result


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


with tempfile.TemporaryDirectory(prefix="sqlite-sync-cli-") as temp:
    root = Path(temp)
    project = root / "project with spaces"
    project.mkdir()
    local = project / "local #%.db"
    remote = project / "remote.db"
    state = project / ".sync"
    with sqlite3.connect(local) as db:
        db.execute("CREATE TABLE messages (id INTEGER PRIMARY KEY, value TEXT)")
        db.execute("INSERT INTO messages VALUES (100, 'UNREAD')")
    config = project / "config.json"
    config.write_text(json.dumps({"local_db": local.name, "remote_db": remote.name,
                                  "device_name": "test laptop"}), encoding="utf-8")
    args = ["--config", str(config)]
    run(root, "status", *args, contains="uninitialized")
    assert not state.exists() and not remote.exists()
    run(root, "sync", *args, contains="PUSH")
    assert digest(local) == digest(remote) == digest(state / "base.db")
    manifest = json.loads((state / "state.json").read_text(encoding="utf-8"))
    assert manifest["device_name"] == "test laptop"
    assert manifest["base_hash"] == digest(local)
    run(root, "status", *args, contains="synchronized")
    # CLI paths override the config and remain relative to the working directory.
    run(root, "status", *args, "--remote", "missing.db", code=1, contains="do not match")
    for path, value in [(local, "READ"), (remote, "READ_KEEP")]:
        with sqlite3.connect(path) as db:
            db.execute("UPDATE messages SET value=?", (value,))
    before = [digest(p) for p in (local, remote, state / "base.db", state / "state.json")]
    run(root, "status", *args, code=2, contains="DIVERGED")
    run(root, "sync", *args, code=2, contains="No files were overwritten")
    assert before == [digest(p) for p in (local, remote, state / "base.db", state / "state.json")]
    run(root, "merge", code=1, contains="Unknown command")
    run(root, "sync", "--local", code=1, contains="Missing value")
    run(root, "sync", "--local", str(local), code=1, contains="required")
    run(root, "sync", "--unknown", "x", code=1, contains="Unknown option")
    run(root, "status", *args, "--config", str(config), code=1, contains="Duplicate option")
    config.write_text('{"local_db": 7}', encoding="utf-8")
    run(root, "status", *args, code=1, contains="Invalid JSON")
    # SQLite validation must leave no sidecars or leaked operation locks.
    assert not list(project.glob("*.lock"))
    assert not list(project.glob("*-wal"))
    assert not (state / "operation.lock").exists()

print("CLI integration checks passed")
