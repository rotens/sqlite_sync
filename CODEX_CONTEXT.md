# sqlite_sync — Codex handoff

This file captures project requirements, implementation decisions, and the state
of the work so development can continue in another checkout without the original
chat or the attachment that supplied the project brief. Read `README.md` and the
actual source alongside this file; update this handoff when the project changes.

## User intent and scope

Build a small, understandable C++20 command-line utility that synchronizes the
SQLite database of a Discord bot between computers through a shared OneDrive
folder. OneDrive is an ordinary filesystem path; there is no cloud API.

The user explicitly requested **Version 1 first; Version 2 later**. Version 1 is
implemented. Do not start row-level merging unless the user asks for it.

The central invariant is: **never silently overwrite divergent databases**.
When the relationship between copies is uncertain, abort and preserve them.

Preferences:

- Primary platform: Windows; keep Linux compatibility.
- C++20, CMake, SQLite C API, and minimal dependencies.
- Small value types, explicit control flow, RAII, `std::filesystem`, and readable
  code the user can explain in a technical interview.
- Avoid unnecessary interfaces, factories, inheritance, and template machinery.
- Work incrementally; explain significant architectural decisions.
- Tests must create temporary databases, never use the user's real bot database.
- No networking, Microsoft Graph, sync server, CRDTs, or bot rewrite.

## Repository layout

```text
CMakeLists.txt
README.md
CODEX_CONTEXT.md
sync-config.example.json
src/
    main.cpp
    SyncManager.hpp / SyncManager.cpp
    DatabaseValidator.hpp / DatabaseValidator.cpp
    FileHasher.hpp / FileHasher.cpp
    Json.hpp / Json.cpp
tests/
    sync_tests.cpp
    cli_tests.py
```

`sync_core` is a library shared by the CLI and the C++ test executable. SQLite3
is the only external build dependency beyond the compiler and CMake. SHA-256 and
the flat JSON string-object parser are bundled. Python 3 is optional for CLI
integration tests. The build does not download dependencies.

## Version 1 behavior

Commands: `sqlite_sync sync` and `sqlite_sync status`.

Options: `--local`, `--remote`, `--state-dir`, `--device-name`, `--config`, and
`--help`. Config keys are `local_db`, `remote_db`, `state_dir`, `device_name`;
all values are strings. Config-relative paths resolve against the config file's
directory. CLI path overrides resolve against the working directory. The default
state directory is `.sync` (beside the config if a config is supplied, otherwise
relative to the working directory).

Let L, R, B be SHA-256 hashes of local, remote, and `.sync/base.db`:

| Condition | Action |
| --- | --- |
| L == B and R == B | No database or persistent metadata changes |
| L != B and R == B | Back up remote, push local, update base/manifest |
| L == B and R != B | Back up local, pull remote, update base/manifest |
| L == R and both differ from B | Update base/manifest |
| Both differ from B and each other | Abort with divergence |

`SyncSnapshot` uses optional hashes to represent missing files. `classify()` is
the filesystem-independent state decision function.

First run, when both base and manifest are absent:

- Local only: initialize remote and base.
- Remote only: initialize local and base.
- Both byte-identical: establish base.
- Both different: initial conflict; do not choose a winner.
- Neither exists: error; do not create an empty SQLite database.

After initialization, a missing local or remote database is an error requiring
explicit recovery. A missing base with an existing manifest, or the converse,
also aborts. Do not turn these cases into implicit reinitialization.

Hashes cover physical file bytes, not logical SQL rows. Equivalent data with
different physical layouts may cause a conservative conflict. File modification
times never determine synchronization direction.

## Safety implementation

- Every existing local, remote, and base database must have the SQLite header and
  pass `PRAGMA integrity_check`. SQLite connections/statements use RAII.
- Validation uses an escaped file URI with immutable read-only mode to avoid
  writing sidecars. Database users must be stopped; immutable mode is not a lock.
- Hash before and after validation; abort if the file changed during inspection.
- Refuse any `-wal`, `-shm`, or `-journal` sidecar, including empty/orphan files.
  Never delete sidecars to bypass this check. Close/checkpoint through SQLite.
- Recognize `<database>.lock` as either a file or directory. Sync acquires
  exclusive directory locks on both DB paths and `.sync/operation.lock`.
  `DirectoryGuard` cleans up its locks/staging during normal unwinding.
- The bot must cooperate: acquire its lock exclusively before opening SQLite,
  retain it throughout DB use, close SQLite, then release it. No bot changes
  have been implemented in this repository.
- Stage copies on the destination filesystem; verify their expected hash,
  validate SQLite, and flush before installing.
- Back up existing destinations before replacement. Also preserve the old base
  and matching manifest. Backups have UTC timestamp/side/counter names and no
  automatic pruning.
- Recheck local, remote, base, and manifest before committing replacements.
- POSIX uses rename for replacement, exclusive link/unlink for a new destination,
  file fsync, and directory fsync. Windows uses MoveFileExW with write-through
  and conditional replacement. Never fall back to deleting the destination.
- Normalize paths; reject supplied symbolic-link endpoints, hard-linked files,
  identical DB paths, and collisions with metadata, locks, sidecars, or staging.
  Windows path collision comparisons are case-insensitive.
- Windows command-line handling uses wmain and UTF-8 conversion.

`.sync/state.json` records format version `"1"`, normalized local/remote paths,
base hash, UTC sync time, and device name. The manifest paths/version/base hash
must match before synchronization. Keep state local to each machine and each
database pair, outside OneDrive.

## Important limits and recovery

There is no atomic transaction across destination, base, and manifest. Their
commit order is destination, base, then manifest. Interruption between base and
manifest installs causes a mismatch and refusal on the next run. Paired old
base/manifest backups support explicit recovery. A crash earlier may leave local
and remote identical, which can be accepted on retry after handling stale locks.
See README for recovery details; never discard metadata just to bypass conflict.

Locks coordinate cooperating local processes, not separate machines through
OneDrive. Avoid simultaneous sync on multiple computers, let cloud transfers
finish, and keep remote files available offline. An external process/cloud client
can change files after the final check. Do not claim distributed locking or
unconditional safety against arbitrary concurrent writers or power loss.

`status` creates no files and does not acquire locks; its results are observations.
`sync` inspects again under its locks. Forced termination may leave stale lock or
staging directories, which require inspection before removal.

## Build and verification

Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

Windows with SQLite headers/library available to CMake:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:/path/to/sqlite"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Alternatively set `SQLite3_INCLUDE_DIR` and `SQLite3_LIBRARY`. Dynamic builds
need the matching SQLite DLL on PATH or beside the executable.

Last verified during implementation: GCC 13.3, CMake 3.28.3, SQLite 3.45.1 on Linux
under WSL1. All 35 C++ cases passed, as did CLI help and Python CLI integration
(three CTest entries total when Python is present). This is a historical result;
rebuild in the new checkout. Native Windows compilation/runtime remain unverified.

Tests cover state decisions, initialization, push/pull, identical changes,
conflict preservation, missing/corrupt files and metadata, locks, sidecars,
backup/staging failures, metadata mismatch, path collisions, JSON escapes, and
SHA-256 known vectors. Python independently checks hashes with hashlib and tests
CLI parsing/config paths/exit codes. Tests do not exhaustively inject crashes,
exercise all race windows, or prove power-loss durability.

Original WSL1 shell sandboxing failed because bubblewrap could not create user
namespaces. Approved unsandboxed execution was used there. This is an environment
limitation, not a requirement to disable sandboxing on the next machine.

## Known follow-up work within Version 1

These are observations, not authorization to start changing code automatically:

- Test natively on Windows, including Unicode paths and replacement behavior.
- Expand dense formatting in source/tests to better match the readability goal.
- CLI preflight returns code 2 for conflicts. A conflict discovered only during
  the second inspection inside synchronize() throws a runtime_error and reaches
  the generic code-1 handler; make conflict exit codes consistent if requested.
- Consider stronger crash/recovery and concurrency testing before relying on the
  utility for important data. Current interrupted-commit tests simulate metadata
  mismatch rather than terminating a process at every commit step.

## Version 2 ideas — deferred

Potential future scope: three-way row merging using base/local/remote, stable
primary keys such as Discord message IDs, non-conflicting changes, explicit
conflict reporting, and output to a separate validated merged database. Possible
schema metadata includes updated_at, updated_by, and deleted_at tombstones.
None of these schema changes, merge commands, or algorithms are implemented.

## Moving to another machine

The user is uploading the repository to GitHub. Git was initialized and the
current branch was `main`; the working tree was clean immediately before this
handoff file was added. Do not assume a push succeeded; inspect Git state as needed.
The user encountered HTTPS password authentication failure and was shown how to
use a fine-grained token scoped to the repository with Contents read/write.
No token belongs in this file, Git history, or chat.

Commit and push this file so it is included in the new clone. On the next machine,
clone the repository, install build prerequisites, then ask Codex:

> Read CODEX_CONTEXT.md and README.md, inspect the current source and Git state,
> and use them as project context. Version 1 is implemented; keep Version 2
> deferred until I request it. Wait for my next development task.

Build output and default `.sync/` are ignored by Git. A source clone will not
include the bot's real databases, backup history, or synchronization ancestor.
Moving an existing real sync setup requires a separate, deliberate transfer of
its data and history; the source-code handoff does not establish a common base.
