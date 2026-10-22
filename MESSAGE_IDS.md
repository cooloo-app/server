# Protocol 3 message identity

`SEND <destination> <uuid> <length>\n<payload>` returns
`OK <end_offset> <uuid> <original_server_ts>`. UUID is 32 lowercase hex characters,
version 4 / RFC variant, generated once with OS randomness by the creating client.
Gate: `send <destination> <uuid>`, exact payload on stdin. Old SEND syntax rejects.
Native greeting is `COOLOO 3`; Noise prologue remains `cooloo-v2` (cryptographic
handshake unchanged). Upgrade clients before reconnecting. No old-record fallback.

Canonical record: `{"ts":1,"from":"…","to":"…","content":"…","id":"…"}`.
UUID is identity; end_offset is still the actual JSONL byte boundary. Preserve UUID
and immutable fields on future replication. Federation is not implemented.

## Commit rules

Under the process-shared registry exclusive lock, authenticate effective sender,
resolve destination, check current permissions, scan all journals for UUID conflict,
then append/fsync once. Same UUID/sender/canonical target/exact content returns the
original offset/time, even after daemon restart or ACK loss. Different fields with
that UUID return `id-conflict`, without historical details. Permissions are checked
before lookup. Private join-request admission keeps its existing exceptional path.

Different UUID with same sender/target/content in the current integer server second
returns `duplicate-content`; never aliases the new UUID to the old UUID. This is D1,
not a sliding 1000ms window. Content is byte-exact; no trimming/normalization. Existing
room control replay remains authoritative; already-applied approvals may reject.

Journals are the only index: linear scan intentionally trades throughput for simple
cross-process crash recovery. Corrupt/missing-ID journals fail closed globally;
repair or migrate offline. No benchmark or scale claim. Explicit same-ID retries
are safe; clients never automatically resend unknown outcomes. Filesystem durability
still depends on the platform honoring fsync; no physical power-loss qualification.

## Offline migration

Stop Native and block/drain gate writers. Back up the entire root, binaries and
service configuration in a root-only directory before running as the data owner:

```
python3 tools/migrate_message_ids.py /var/cooloo
python3 tools/migrate_message_ids.py /var/cooloo --apply
```

The first invocation validates without writing. Apply persists an immutable staging
manifest and replacement files before switching any live data. Re-run apply after
interruption, with writers still stopped: staged UUIDs and epochs are reused.
Existing valid IDs survive; invalid/duplicate IDs fail. Legacy duplicate records
remain distinct. All original semantic fields and record order survive. ACL journal
start offsets are translated to new byte boundaries, not reset/replayed from zero.
Every stream generation rotates, including empty inboxes. Unrelated metadata,
relations, credentials and private sync files remain unchanged.

Validate record counts/fields/UUIDs and ACLs, then install both binaries and restart.
Do not re-run migration after accepting new writes (the manifest deliberately detects
history changes). Keep `.message-id-migration-v1` protected with the data and backup.
New clients reset old cache prefixes and read watermarks on the new generation,
refetch from offset zero; old remote read-state generations are not imported.
Drafts, identity, pins, configuration and pending-send records are not deleted.
Local old cache bytes are archived for recovery, not displayed or used for cursors.

Rollback is coordinated: stop all writers and restore matching old binaries AND
complete pre-migration data; restoring after new messages arrive needs a separate
loss/reconciliation decision. Never run an old binary against migrated journals.
