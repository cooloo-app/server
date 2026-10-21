# User-private sync files and message stream epochs

The server stores opaque bytes and revisions, not read-state/config/script logic.
`self` is the authenticated effective identity (pure mirrors share the parent).
`rooms/<id>/scripts/**` and `rooms/<id>/config/**` belong to that user, NOT the
room or its owner. Losing room membership does not revoke access to one's private
files. `config` is singular; old `configs` paths are not auto-renamed or watched.
There is no room-member shared client script scope. Future room-owned
`server-config/server-scripts` require a separate owner-admin deployment runtime;
that runtime and scope are intentionally not implemented here.

## Native commands

Existing small-file commands remain compatible:

```
FLIST self
FGET self <path>
FPUT self <path> <base-revision> <lowercase-hex|->
```

New bounded, stop-and-wait upload and streamed download:

```
FBEGIN self <path> <base-revision> <raw-size>   -> READY 0
FCHUNK <offset> <hex>                        -> READY <next-offset>
FCOMMIT                                     -> FILE ... then END
FREAD self <path>                            -> FILE ... / DATA ... / END
FDELETE self <path> <base-revision>           -> DELETED ... then END
```

Chunks are 1..2048 raw bytes, ordered and contiguous. Zero-byte upload uses
FBEGIN size=0 then FCOMMIT. Nothing is published until complete FCOMMIT; disconnect
frees staging. A connection cannot change AS or use message streams during file
transfer. Final commit rechecks delegation and CAS. GET/READ output is a consistent
temporary snapshot pumped with socket backpressure; authorization is rechecked.

Records:

- `FILE <path> <revision> <raw-size>`: LIST/PUT/FBEGIN+COMMIT or FREAD header.
- `FILE <path> <revision> <raw-size> <hex|->`: legacy FGET (up to 4096 bytes).
- `DATA <raw-offset> <hex>`: FREAD data, at most 2048 raw bytes/line.
- `DELETED <path> <revision>`: persistent tombstone, including in LIST and GET.
- `END`: successful operation end, except READY acknowledgments.
- `ERR <code> <detail>`: request failure, no END.

Revision 0 is create-only. Stale writes/deletes return `ERR conflict`; missing GET
returns `ERR not-found`. Recreating a deleted file requires its tombstone revision.
After uncertain commit, GET again; do not blindly replay a stale snapshot.

## Limits and persistence

- 1 MiB inclusive per file; old FPUT/FGET retain 4 KiB framing limits.
- Default 4096 entries (including tombstones) and 64 MiB live bytes per identity.
- `COOLOO_SYNC_FILES` (1..65536) and `COOLOO_SYNC_BYTES` (1..1073741824) override
  quotas; invalid values use defaults. Deletions retain entry quota to prevent ABA.
- Paths: 120 ASCII bytes, alphanumeric plus `._-/`; no empty/dot/dot-dot components.
- Native only; CLI/script request wrappers and SSH file APIs are not added.

`sync/<effective-id>/<hex-path>` holds `V2 <revision> <deleted:0|1>\n` plus bytes.
Old revision-only envelopes remain readable and migrate on write. Stable registry
lock covers authorization, quota accounting, CAS and publication. fsync envelope,
rename, then fsync directory. Old binaries cannot read V2 envelopes: do not downgrade
without a reviewed migration. Tombstones are not automatically garbage-collected.

## Client reconciliation

Only private `scripts/` and `config/` under authenticated room cache directories
are watched. No automatic execution, config activation, or `configs/` migration.
Each loaded room alternates read-state and folder sync. A completed LIST precedes
local scanning and remote reads. Exact-byte per-file baselines persist under
`.cooloo-sync/`; missing baseline never implies local deletion. Modification and
removal use CAS. Concurrent edits/deletion preserve the local variant under
`.cooloo-conflicts/` before adopting remote bytes (local deletion gets a note).
Baselines record the uploaded snapshot, not later edits. Failed or uncertain
operations are reconciled again on the next cycle. Symlink aliases are rejected;
scan depth 16 and 8192 entries are client bounds. Filesystem work is synchronous
and bounded by these caps; large workspace UI performance is not benchmarked.
One GUI process per local cache directory; use distinct device configurations.

## Message stream generation

`STREAM <room-or-inbox-id>` returns `STREAM <canonical-id> <64-hex-generation>`.
It checks message read permission. The epoch is persisted in `.generation` under
the stream directory; normal restarts do not rotate it. The server does not parse
read watermarks. New clients query before cache loading, archive/reset old message
cache on epoch change, and bind local JSON v2 to the epoch:

```
{"version":2,"generation":"<64 hex>","read_offset":123}
```

Shared state lives at `self/read/<generation>/read-state.json`. Namespacing keeps
offline clients from overwriting another epoch's read-state. Only matching epochs
are merged by client max. First migration binds the existing local watermark;
a changed epoch resets to zero without treating newly downloaded history as read.
Older servers use local/legacy behavior; older clients do not gain epoch protection.

**Backup restoration is an explicit administrative operation**, not detectable in
general from offsets (same-size replacement is possible). Stop Native and gate
writers, restore the approved snapshot, then run:

```
python3 tools/rotate_stream_generations.py /var/cooloo          # preview
python3 tools/rotate_stream_generations.py /var/cooloo --apply  # explicit rotation
```

Do this before reconnecting clients whenever history is replaced/truncated/restored.
Never rotate during routine deployment. Offline tool uses registry lock, atomic
replacement and directory fsync; it does not modify messages or private workspaces.
Independent manual edits of history while clients remain connected are unsupported.
