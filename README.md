# sqlite_sync — Version 1

A small C++20 command-line tool for synchronizing two SQLite database files through
an ordinary shared folder, such as OneDrive. It compares SHA-256 content hashes
against a saved common ancestor. Divergence aborts synchronization and preserves
both databases. There is no row merging in this version.

## Build

Requirements: CMake 3.20+, a C++20 compiler, and SQLite3 development files. SHA-256
and the small flat JSON reader are included; no dependencies are downloaded by
the build. Python 3 is optional, for additional CLI tests only.

Linux (with SQLite development files installed):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows, from a Visual Studio developer terminal with SQLite installed:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:/path/to/sqlite"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Alternatively, provide `SQLite3_INCLUDE_DIR` (containing `sqlite3.h`) and
`SQLite3_LIBRARY` (the matching SQLite library) as CMake cache variables. If using
a dynamic SQLite library, make its DLL available beside the executable or on
`PATH`. The Windows executable is `build/Release/sqlite_sync.exe` with a
multi-configuration generator. Windows has platform-specific Unicode argument
handling and replacement code; automated verification so far has been on Linux.

## Usage

Stop the bot and every other database user first. Let OneDrive finish downloading
the remote file. Keep the remote file available offline. Run:

```sh
./build/sqlite_sync status --local data/reading_manager.db --remote /path/to/OneDrive/reading_manager.db
./build/sqlite_sync sync --local data/reading_manager.db --remote /path/to/OneDrive/reading_manager.db
```

On Windows:

```powershell
.\build\Release\sqlite_sync.exe sync --local "C:\Projects\DiscordBot\data\reading_manager.db" --remote "C:\Users\you\OneDrive\DiscordBot\reading_manager.db"
```

Optional configuration (`sync-config.json`):

```json
{
  "local_db": "data/reading_manager.db",
  "remote_db": "C:/Users/you/OneDrive/DiscordBot/reading_manager.db",
  "state_dir": ".sync",
  "device_name": "desktop"
}
```

```sh
./build/sqlite_sync status --config sync-config.json
./build/sqlite_sync sync --config sync-config.json
```

Configuration paths are relative to the configuration file's directory. Without
configuration, paths (and the default `.sync`) are relative to the working
directory. `--local`, `--remote`, `--state-dir`, and `--device-name` override
configuration; command-line paths are always relative to the working directory.
Unknown/duplicate options, unknown config keys, and malformed JSON are errors.
Config values must be strings. `status` performs validation and hashing without
creating files or changing metadata.

Exit codes: `0` success, `1` error/refused unsafe operation, `2` detected conflict.

## Decisions

`L`, `R`, and `B` are the hashes of local, remote, and `.sync/base.db`.

| Condition | Action |
| --- | --- |
| `L == B`, `R == B` | Nothing; leave metadata unchanged |
| `L != B`, `R == B` | Back up remote, push local, update base and state |
| `L == B`, `R != B` | Back up local, pull remote, update base and state |
| `L == R`, both differ from `B` | Update base and state |
| Both differ from `B` and from each other | Abort; preserve every copy |

First use, with neither base nor manifest:

- Only local exists: initialize remote and base from local.
- Only remote exists: initialize local and base from remote.
- Both exist and are byte-identical: establish the base.
- Both differ: abort with an initial synchronization conflict.
- Neither exists: abort; do not create an empty SQLite database.

After initialization, a missing local or remote database is an error. Restore
the intended file explicitly before retrying. Missing or mismatched metadata is
also an error; it is never treated as permission to establish a new ancestor.
Hashes cover exact file bytes, so logically equivalent databases with different
physical layouts can still cause a conservative conflict.

## Safety and operating assumptions

- Keep a **separate local state directory per database pair, per machine**, outside
  OneDrive. It contains `base.db`, `state.json`, and `backups/`. Do not share or
  casually delete it: the ancestor is what makes divergence detectable.
- Every existing database must have a SQLite header and pass
  `PRAGMA integrity_check`. Validation uses SQLite's immutable read-only mode,
  which cannot create sidecars. A corrupt local, remote, or base aborts the run.
- Any `-wal`, `-shm`, or `-journal` file causes refusal, including orphan sidecars
  beside a missing main file. A main-file copy alone can omit committed WAL data.
  Close all SQLite users cleanly; if needed use SQLite to checkpoint while all
  other users are stopped, then close that connection. **Do not delete sidecars
  manually.** WAL-mode database headers without sidecars are supported.
- A bot lock is `<database path>.lock`. The bot must honor the lock before opening
  SQLite, acquire it exclusively, retain it for its entire database use, close
  SQLite before releasing it, and refuse startup if it already exists. The tool
  recognizes either a file or directory and creates its own lock directories on
  **both** database paths, plus `.sync/operation.lock`. These coordinate cooperating
  local processes; the utility cannot detect an idle bot that ignores locks.
- Stop database use and avoid simultaneous synchronization on other computers.
  These filesystem locks **are not distributed locks**. OneDrive may download or
  upload asynchronously, and this utility cannot atomically coordinate with its
  client or prevent an external process from changing a file after a final check.
  Let cloud uploads finish before switching computers; inspect any OneDrive
  conflict copies separately. The tool can detect divergence in the two files it
  observes; it cannot guarantee that OneDrive has delivered the newest remote file.
- Each replacement is staged on the destination filesystem, hashed, validated,
  and flushed. Existing destinations get verified, flushed backups before
  replacement. All three hashes and the manifest are checked again before commit.
  Symbolic links as supplied endpoints, hard-linked files, identical database
  paths, and overlapping metadata/sidecar/lock/staging paths are refused.
- POSIX uses atomic `rename` for replacements, exclusive `link` for a new file,
  and directory flushing. Windows uses `MoveFileExW` with write-through and
  replacement flags as appropriate; it never falls back to deleting the target.
  Filesystem support determines atomicity and power-loss durability. Failure aborts
  with an error, and already completed replacements are reported through the
  subsequent status/metadata checks rather than being silently rolled back.
- There is no atomic transaction spanning local, remote, base, and state. A crash
  after copying one side usually leaves identical databases that can be accepted
  on retry. A crash between base and manifest installation leaves inconsistent
  metadata and **aborts on the next run**. All backups are retained without pruning.

## Inspecting conflicts and recovering interruptions

On conflict, run `status` and preserve local, remote, base, and metadata. Version 1
has no `resolve` command and never chooses a divergent side for you. Make any
manual reconciliation on separate copies, validate it, and retain the originals.

Backups are named using UTC timestamps, side (`local`, `remote`, or `base`), and a
collision counter. A replaced base also has a matching `.state.json` backup.
After an interrupted metadata commit, preserve the entire `.sync` directory and
both current databases first. Inspect the current manifest and the paired backups;
recover a matching base/manifest pair whose `base_hash` equals that backup's
SHA-256. Then run `status` to reassess current local and remote changes. Do not
edit a hash to make an unrelated ancestor appear valid, or delete metadata to
bypass a conflict.

A forced termination can leave `.lock`, `operation.lock`, or
`<database>.sqlite-sync-stage` directories. Confirm no bot or sync process is
running, preserve any staged database that could be useful, and remove only those
stale locks/staging directories before retrying. Never remove a live process's
lock.

## Source and tests

- `DatabaseValidator`: sidecar checks and read-only SQLite integrity validation.
- `FileHasher`: streaming SHA-256 with known-vector tests.
- `Json`: flat string-object reader/writer for config and state.
- `SyncManager`: state classification, locks, verified backups, staging, and commit.
- `main`: commands, configuration, output, and exit codes.

The C++ suite creates temporary SQLite databases and covers all Version 1 states,
first-run cases, corrupt copies, missing files/metadata, locks, sidecars, backup
and staging failures, interrupted metadata, path collisions, and SHA-256 vectors.
Optional Python CLI tests check configuration resolution, hashes independently
with `hashlib`, conflict preservation, argument errors, and exit codes.
