# Protocol 2 — v0.2.1

This release implements public-key IDs, file-based inboxes and simple private
rooms. Both the Native daemon and SSH gate must be upgraded together. There is
no protocol-1 compatibility shim or legacy-history migration.

## Simple private rooms

See [PRIVATE_ROOMS.md](PRIVATE_ROOMS.md) for commands, member blocks, join-message
rules, storage and limitations. **Any registered user can make any room public**
in this temporary policy; do not use it as a confidentiality guarantee.
`public -> private` adds this room's historical senders, retaining owner and
existing members (maximum 200 distinct members).

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
  identity can read/write public rooms. Only the recipient can read a user
  inbox, including a child inbox: its parent does not gain read access.
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
SEND returns `OK <end-offset>`. Errors start with ERR. Use explicit offsets
for durable catch-up. Private room metadata requires membership; user display
metadata remains public.

## SSH gate

Administrator binds an SSH key to an existing public ID with a restricted forced
command `cooloo __gate <public-id>`. SSH key association is an explicit admin
operation, not inferred from matching nicknames. Never allow the remote caller
to choose the forced-command ID.

Gate commands: `register <nickname>`, `whoami`, `rooms`, `create <nickname>`,
`avatar <slot> <child-id> <nickname>`, `rename <id> <nickname>`, `meta <id>`,
`send <id> [text...]`, `read <id> <offset>`, `follow <id> <offset>`,
`join <room-id>`, `room-mode <room-id> public|private`.
Use stdin for exact multiline message bytes. Gate tokenizes command arguments
by whitespace; native registration supports spaces in nicknames.
Local administration uses `cooloo --as <public-id> <command>`; this entry point
is not accepted through SSH_ORIGINAL_COMMAND.

## Validation boundaries

See the planner release/deployment handoff for platform and production validation.
`make test` covers model, Native and gate behavior in isolated environments;
these tests alone do not verify actual sshd or full GUI operation.
Registry lookup is intentionally file-based and linear; no scale benchmark.
