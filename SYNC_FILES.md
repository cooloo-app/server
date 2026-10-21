# Opaque sync files (Native Protocol 2)

Server stores bytes, never parses read-state, scripts or configuration. Initial
scope `self` means the AS/effective identity, including pure mirror credentials.
Every operation rechecks existing read/write delegation. Files are account-private,
not shared with room members merely because their path contains a room ID.

```
FLIST self
FGET self <path>
FPUT self <path> <base-revision> <lowercase-hex-bytes|->
```

Success: `FILE <path> <revision> <raw-size>` (PUT/LIST), or
`FILE <path> <revision> <raw-size> <hex|->` (GET), followed by `END`.
LIST emits zero or more FILE lines. Errors are a single `ERR` line, without END.
`-` encodes an empty file. Revision 0 means create-only; existing revisions start
at 1. Stale PUT returns `ERR conflict` without replacing data. Missing GET returns
`ERR not-found`. No unconditional overwrite, delete or subscription in this version.
After timeout/connection loss, GET again: a PUT may already have committed.

Limits: 4096 raw bytes/file, 128 files/effective identity (512 KiB payload total),
120 ASCII path bytes; alphanumeric, `.`, `_`, `-`, `/`; no empty, `.` or `..`
components. The small initial cap deliberately fits the existing line transport;
this is not yet the proposed 1 MiB transfer protocol. Binary content is supported.
Native only; SSH gate and public client CLI/script request wrappers are unchanged.
Use a dedicated connection, not a FOLLOW stream, to associate responses.

Storage: `sync/<effective-id>/<hex-path>` contains decimal revision + LF + opaque
bytes. A shared registry lock covers authorization/CAS/publication; fsync temporary
file, rename the entire envelope, fsync directory before acknowledgment. LIST is
bounded and consistent under the lock. Content and revision cannot tear separately.
Include `sync/` in offline whole-root backups; never restore message histories and
sync state independently. No server-side JSON semantics or execution.

Client first application: `rooms/<room-id>/read-state.json`, canonical
`{"version":1,"read_offset":N}`. Client computes max, persists locally before
updating unread, and retries conflicts by GET/merge/CAS. Device mute stays in a
local sidecar. Download offsets are never advanced by shared read watermarks.
Initial sync polls loaded rooms round-robin, at most one request connection at a
time; unknown-command falls back to local state until reconnect.

Limits: no cross-server replication, room-member scope, automatic scripts/configs
folder watcher, deletion propagation, or stream-generation protocol. Current
messages are append-only and room IDs are not reused. Restoring older history or
reusing a stream ID is NOT supported by offset-only read-state: stop clients and
plan a coordinated new stream/cache namespace rather than silently retaining old
watermarks. Concurrent GUI processes sharing the exact same local cache directory
are not a supported merge boundary; use separate device config directories.
