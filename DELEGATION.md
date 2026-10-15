# Message delegation and filtered sent history — v0.2.2

Requires matching upgraded daemon, SSH gate and client.
No `full` permission and no new identity extension block. Existing private-room
member blocks are unchanged.

## Owner grants, child selects

A must first create B as an existing avatar using the existing `avatar` operation.
This version does not bind already-registered independent users as avatars.
Slot numbers (001–899) carry no implicit permission.

Using A's actual credential:

```sh
cooloo delegate <B-id> read
cooloo delegate <B-id> write
cooloo delegate <B-id> read,write
cooloo delegate <B-id> none
```

Each command **replaces** the permission set; `none` revokes both. It is not an
additive grant. B cannot grant/revoke on A's behalf, even with both permissions.
No transitive delegation. Read and write are independent:

- read: A's inbox READ/FOLLOW, A's accessible room READ/FOLLOW, ROOMS,
  visible META/METAFILE, and filtered SENT records from A.
- write: SEND as A, subject to A's ordinary destination permissions. Addressed
  room controls sent via SEND retain their current effects (e.g. join approval).
- neither permission grants AVATAR, DELEGATE, RENAME, CREATE, or ROOMMODE as A.
  HELLO/WHOAMI remain tied to the actual credential.

B defaults to B. Use an explicit prefix to act as A:

```sh
cooloo send <C-id> 'from B'
cooloo --as <A-id> send <C-id> 'from A'
cooloo --as <A-id> read <A-id> 0
cooloo --as <A-id> follow <A-id>
cooloo --as <A-id> sent <C-id> 0
cooloo --as <A-id> history
```

Place global flags before the command. The transmitted `from` is the effective
identity. CLI cache identity is the effective identity, not the login credential.
`whoami` is an authentication operation, not a delegated profile query; use it
without `--as` (META queries the effective profile explicitly).

## Trust boundary

Authoritative grants live under `$COOLOO_HOME/authz/<owner-id>/<child-id>`:

```text
DELEGATE-1 <permission-bits>
```

Bit 1 = read, bit 2 = write. Files are 0600 in 0700 directories, outside mutable
user properties. Missing or invalid records deny access. No permission is inferred
from child owner/slot metadata. Changing child properties alone cannot create a
grant; the owner must authenticate and explicitly grant. Grant creation checks
that the recipient currently is the owner's avatar; revocation does not depend
on child properties remaining unchanged.

Future Lua property APIs MUST NOT expose `authz`, identity registry security
operations, arbitrary filesystem paths, or auth-context setters. This version
adds no Lua filesystem writer. Server-admin filesystem access remains trusted.

Both transports bind the actual authenticated identity in model request context.
Authorization is rechecked under the same registry lock as each read/send/list
operation, including READ/FOLLOW pumps and completion of a SEND payload. Grants
are atomically replaced under that lock; revocation removes the grant. Already
transmitted or queued data and client caches cannot be recalled. Files are fsynced
before replacement, but existing directory-entry power-loss limitations remain.
All backup/restore operations must include authz; never restore stale grants alone.

## Native protocol

```text
AS <effective-user-id>              -> OK
DELEGATE <child-id> read|write|read,write|none -> OK
SENT <peer-user-id> <scan-byte-offset>
```

AS selects self or an explicitly delegated identity with at least one permission.
Each operation then enforces the particular permission. Selection cannot change
while a READ/SENT transfer or FOLLOW subscription exists; use another connection.
Authentication is never replaced. AS errors must not be ignored by clients.

SENT only accepts user inboxes, never rooms. It returns ordinary MSG frames but
only for `from == effective identity`, followed by `END <scan-byte-offset>`.
The scan cursor advances over unmatched records as well, including when nothing
matches. Invalid/non-boundary/beyond-end offsets fail. Peer inbox contents are
filtered server-side; clients cannot request a different sender. No server index,
message copy, contact record, or whole-server scan is introduced. Source byte
positions can reveal peer inbox size/activity, but not unrelated message content;
this is a deliberate consequence of reusing physical scan offsets in this version.

Filtered records are not a contiguous peer inbox: NEVER put them in the normal
room JSONL cache. (peer ID, source end offset) identifies a returned record in the
append-only log. Log replacement/truncation is outside the supported cursor model.

SSH gate accepts `--as <effective-id>` before its command, plus
`delegate <child-id> <permissions>` and `sent <peer-id> <offset>`. Remote --as only
selects an authorized effective identity; it never selects the authenticated one.
The outer local administrative `cooloo --as <authenticated-id> ...` remains a
trusted local filesystem administration tool, not remotely callable dispatch.

## Recovery scope

Native CLI `sent` prints JSONL and prints the next scan offset to stderr. `history`
reads its own inbox, discovers peers from received messages, queries only those
peer inboxes, deduplicates by (destination, source offset), and prints a timestamp-
sorted JSONL snapshot. Equal timestamps use destination and offset for deterministic
ordering, not a cross-inbox causal-order guarantee. CLI limits are 10,000 merged
messages and 256 peers; exceeding a limit fails explicitly. It currently uses all
received records within these bounds rather than a time-window filter.

GUI has one effective identity per window: enter `/as <A-id>` or `/self` in the
message input. All new connections select the window identity before use; the
status bar shows shortened effective/login IDs. Switching clears in-memory views;
READ caches are partitioned by effective identity. There is no separate identity
picker or multiple identity panes in this version. Read-only delegation works for
browsing; write-only is primarily usable through CLI because GUI lists need read.

After inbox catch-up, GUI discovers up to 256 peers from its bounded recent message
buffer and fetches SENT sequentially. Filtered outgoing messages are merged in the
inbox view without advancing its READ/FOLLOW checkpoint. This is an online,
in-memory view, not a durable sparse-cache implementation. Reconnect refreshes
queries; existing-peer outgoing changes on another device are not continuously
followed in this version. Ordinary incoming messages continue through FOLLOW.

**Known and accepted gap:** a peer that has never sent to this identity is not
automatically discovered, even if this identity has sent to it. Direct `sent` works
if the peer ID is known. Do not describe history as complete multi-device recovery.
No shared read receipts, contact/room-subscription sync, or E2EE is included.

## Validation

`make -C server test` and `make -C client test` cover independent permissions,
default-vs-effective sender, escalation rejection, metadata tampering without
grants, invalid grant rejection, server-side filtering, empty scans/bad offsets,
merged recovery/accepted discovery gap, cross-transport revocation, live delegated
FOLLOW cancellation, effective-identity cache paths, and mocked GUI state flow.
Real GUI, Windows/macOS builds, real sshd and production rollout are separate.
