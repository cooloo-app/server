# Protocol 2 — ordinary avatars and pure mirrors

This release implements public-key IDs, file-based inboxes and simple private
rooms. Both the Native daemon and SSH gate must be upgraded together. There is
no protocol-1 compatibility shim or legacy-history migration.

## Simple private rooms

See [PRIVATE_ROOMS.md](PRIVATE_ROOMS.md) for commands, member blocks, join-message
rules, storage and limitations. **Any registered user can make any room public**
in this temporary policy; do not use it as a confidentiality guarantee.
`public -> private` adds this room's historical senders, retaining owner and
existing members (maximum 200 distinct members).

## Message delegation

See [DELEGATION.md](DELEGATION.md) for parent access to ordinary children, pure
mirror credentials, identity discovery, detach and filtered SENT limitations.
Parents default to read/write on ordinary children. Mirrors default to the parent
message identity and cannot send or receive as themselves. Old child relationships
and authz must be reset offline; there is no legacy delegation compatibility.

## Build and verify

```
make all
make test
```

Tests use temporary directories and loopback listeners. `make test` runs the
Noise known-answer vector, native protocol integration and file-model tests.
Old `selftest.sh` / `nettest.sh` scripts describe protocol 1 and are not the
current test entry point.

## Identity and storage

- User ID: lowercase hex of the actual X25519 public key (64 characters), not
  its fingerprint. Nicknames may repeat; `-` is reserved by the greeting.
- A root user can register an unused child key in slots 001–899. Children
  authenticate independently. `<owner-id>:001` resolves to the child's ID;
  `:000` resolves to the root. Slots 900–999 are reserved.
- Public rooms have random 64-character IDs and an owner. Every registered
  identity can read/write public rooms. User inboxes are readable by the recipient
  and a parent with read access; mirrors use their parent's message identity.
- Default root: `~/.cooloo/server`; `COOLOO_HOME` overrides it.
- Files: `rooms/<id>/.meta` and `rooms/<id>/messages.jsonl`. Metadata is atomically
  replaced; message writes use a shared cross-process file lock and fsync.
- JSONL fields: `ts`, `from`, `to`, `content`. Timestamp is server-assigned;
  sender is authenticated. Content is 1–4096 UTF-8 bytes, NUL excluded.
- Offset is the server JSONL byte boundary. Corrupt/incomplete server tails
  fail closed; they are not silently skipped or overwritten.
- Stop all writers before copying the root for backup. It contains private keys.

## Native commands

Noise prologue `cooloo-v2`; greeting `COOLOO 2 <nickname|->`.

```
HELLO <nickname>
WHOAMI
CREATE <nickname>
AVATAR <slot> <child-public-id> <nickname>
MIRROR <slot> <child-public-id> <nickname>
DETACH <child-public-id>
DELEGATE <child-public-id> read|write|read,write|none # parent's access to child
IDENTITY                     # actual credential and effective identity
AS <effective-id>
SENT <peer-user-id> <scan-offset>
RENAME <id-or-alias> <nickname>
ROOMMODE <room-id> public|private
META <id-or-alias>             # INFO id type owner-or-- slot nickname
METAFILE <id-or-alias>         # META <canonical JSON metadata>
ROOMS                        # accessible rooms + own inbox, then END
SEND <id-or-alias> <length>\n<raw payload>
READ <id-or-alias> <offset>   # from boundary; no legacy -N
FOLLOW <id-or-alias> [offset] # omitted offset = current tail
PING
```

Messages: `MSG <end-offset> <JSON-byte-length>\n<JSON>\n`. READ ends with END.
SEND returns `OK <end-offset>`. Errors start with ERR. `PING` returns `PONG`;
long-lived clients should send it every 30 seconds (idle input timeout: 600 seconds).
A heartbeat can interleave complete records during READ/FOLLOW but never a MSG
header/body pair. ROOMS is a snapshot, not a change subscription; clients must
refresh it explicitly. FOLLOW checks for updates on the 100ms daemon tick.
Use explicit offsets
for durable catch-up. Private room metadata requires membership; user display
metadata remains public.

## Opaque sync files

Native `FLIST/FGET/FPUT self` provide versioned, atomic, account-private files.
See [sync file contract](SYNC_FILES.md) for framing, limits, CAS, persistence and
client-owned read-state semantics. Server does not interpret file contents.

## SSH gate

Administrator binds an SSH key to an existing public ID with a restricted forced
command `cooloo __gate <public-id>`. SSH key association is an explicit admin
operation, not inferred from matching nicknames. Never allow the remote caller
to choose the forced-command ID.

Gate commands: `register <nickname>`, `whoami`, `rooms`, `create <nickname>`,
`avatar <slot> <child-id> <nickname>`, `rename <id> <nickname>`, `meta <id>`,
`send <id> [text...]`, `read <id> <offset>`, `follow <id> <offset>`,
`join <room-id>`, `room-mode <room-id> public|private`, `identity`,
`mirror <slot> <child-id> <nickname>`, `detach <child-id>`,
`delegate <child-id> read|write|read,write|none`, `sent <peer-id> <offset>`.
Use stdin for exact multiline message bytes. Gate tokenizes command arguments
by whitespace; native registration supports spaces in nicknames.
Local administration uses `cooloo --as <public-id> <command>`; this entry point
is not accepted through SSH_ORIGINAL_COMMAND.

## Validation boundaries

See the planner release/deployment handoff for platform and production validation.
`make test` covers model, Native and gate behavior in isolated environments;
these tests alone do not verify actual sshd or full GUI operation.
Registry lookup is intentionally file-based and linear; no scale benchmark.
