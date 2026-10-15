# Simple private rooms — v0.2.1

This is **not a security boundary against registered users**: deliberately, any
registered identity may change any room to public, exposing its entire history.
Both production entry points must use this version; permission redesign is not
included. Do not downgrade old server binaries onto access-aware data.

## Commands

Native client:

```sh
cooloo join <room-id>
cooloo room-mode <room-id> private
cooloo room-mode <room-id> public
cooloo send <room-id> '@<room-id> join-approve <applicant-user-id>'
```

Substitute actual 64-character lowercase IDs inside the quoted message. `join`
sends `@<room-id> join-request` using ordinary SEND. Approval intentionally has no
request ID. SSH gate offers the same `join` and `room-mode` commands, and `send`
accepts the same control messages. Native mode command is
`ROOMMODE <room-id> public|private`, returning `OK` or `ERR`.

Only the exact destination's `@<room-id>` prefix followed by spaces/tabs is
reserved. Commands accept one or more spaces/tabs between tokens, but no trailing
whitespace/newlines or extra fields. Unknown/malformed addressed commands fail;
other @targets remain ordinary message text. There is no custom room program yet.
Use CLI arguments or `printf`, not newline-appending `echo`, for control payloads.

## Rules

- Owner is a member and counts toward the maximum **200** members. No admins.
- Any current member, including the owner, may approve a pending applicant.
  Public readers/writers do not automatically become members.
- Member identity is the full authenticated user ID. Avatars are independent.
- Each member has an opaque 0–256 raw-byte block, initially empty. No block
  read/write API or Lua interpretation exists in this version.
- Private rooms permit only members to read, FOLLOW, send ordinary messages,
  list the room, or query its metadata. A nonmember may send an exact join request.
- Accepted requests are visible in room history; failed operations are not appended.
  Approval messages are also the durable grant records. Repeated approval of an
  existing member by a member succeeds without appending or consuming another seat.
- A request requires **more than 86400 seconds** since the previous accepted
  request for that room/identity. Rejected retries do not refresh the timestamp.
  Server time is authoritative and state survives restart.
- Pending requests expire after seven days. Up to 1024 distinct applicants with
  recent request records are tracked per room; approved requests retain their
  timestamp until expiry. Full rooms reject new requests. This bounds replay
  state, not the append-only log, and does not prevent multiple-identity spam.
- Approval targets the applicant's current pending request; there is no request
  identifier. An old reply can therefore approve a later renewed application.
- Any registered user may switch either direction, including nonmembers and
  users who do not know the private room's metadata. Unregistered users may not.
- On **public -> private**, add the distinct authenticated `from` user IDs from
  **this room's entire message history** to the member set. Accepted control
  messages (including join requests) count; rejected messages do not. Other rooms,
  inbox traffic, registration, reading and switching mode do not grant membership.
  Owner and existing members/blocks persist, even if they have never sent a message.
  More than 200 members in this union rejects the conversion
  without changing the mode. Calling `private` on an already-private room is a
  no-op, not another snapshot.
- On **private -> public**, everyone registered can access **all old history**.
  Members and blocks persist. Public content/cached copies cannot be recalled.
- Full history is available to current members. There is no kick, leave,
  ownership transfer, per-period history, or end-to-end encryption.

Every READ/FOLLOW pump checks current access. SEND validates immediately before
append under the shared cross-process registry lock. Already buffered/transmitted
bytes cannot be retracted. On public -> private, followers who are neither
existing members nor historical senders lose access on the next read pump.

## Storage and crash semantics

`rooms/<id>/.meta` contains `"access":1` for the access-aware room schema. Legacy
metadata without that field explicitly denotes an old public room. Missing or
malformed `.access` for an access-aware room fails closed; it is never interpreted
as public. Mode/member storage is separate from client-cached metadata.

`.access` is a bounded, atomically replaced text snapshot:

```text
COOLOO-ACCESS-1 <private:0|1> <journal-start-byte-offset> <member-count>
<user-id> <block-byte-length> <lowercase-hex-block>
...
```

Empty blocks have length zero and no hex data. Encoding allows arbitrary raw
bytes; 256 bytes require 512 hex characters. It is server-internal, not a client
interface. No member secrets should be assumed.

Accepted join/approval messages after the journal start in `messages.jsonl` are
replayed for pending requests and grants. An approval's existing durable JSONL
append **is** its commit, avoiding a separate member-file/message-file commit.
The registry exclusive lock serializes validation, capacity checks, mode changes
and append. Replay is intentionally linear; no large-room performance claim.
Mode saves materialize members (including replayed grants) and preserve blocks;
replay remains idempotent. Clients still cache the unchanged contiguous JSONL.

Legacy access activation writes a public owner-only access snapshot first, then
marks `.meta`, with the start offset at the old log end. Old literal control-like
messages do not become retroactive grants. A crash before the schema marker leaves
an explicitly legacy public room; a marked room with missing access fails closed.
New rooms start public and initialize owner membership.

Files are fsynced before rename/acknowledgment, as in the existing file model;
full directory-entry durability across power loss is not promised. Stop all
writers for backup and include `.meta`, `.access`, and the complete message log.
Do not edit/truncate the journal or use old server binaries on access-aware data:
old readers do not enforce this feature and may fail or bypass access rules.
Both Native daemon and SSH gate must be upgraded together.

## Verification

`make test` includes `tests/private_room_test.py` (isolated gate processes,
restart/replay, cooldown/expiry, malformed commands, metadata ACL, blocks,
legacy activation, snapshot limit, concurrent final-seat approval). The sibling
client's `make test` covers the actual Native CLI and SSH gate in both directions.
These tests do not constitute real sshd deployment, GUI, macOS, Windows, power-loss
or scale validation.
