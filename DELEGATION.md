# Ordinary avatars and pure mirror credentials

A is the parent; B is its child. Relationships are single-level and created only
for previously unused public keys. Independent registered accounts cannot be
claimed. Slots 001–899 resolve to B, never to A; 000 resolves to A.

## Ordinary avatar

Using A's actual credential:

```sh
cooloo avatar 001 <B-id> <nickname>
cooloo delegate <B-id> read
cooloo delegate <B-id> write
cooloo delegate <B-id> read,write
cooloo delegate <B-id> none
```

B is an independent message identity. A starts with read/write access **to B**.
`delegate` replaces A's access to B; it does NOT grant B access to A. Read and
write are independent. A may read B's inbox directly or select `--as B` to use
B's message permissions (including B's private-room memberships and SENT).
`--as B send ...` records B as sender. B cannot turn off A's access.

Read covers inbox/accessible room READ/FOLLOW, ROOMS, visible metadata and
filtered SENT. Write covers SEND, including existing addressed room controls.
Neither permits administration as another identity: AVATAR, MIRROR, DETACH,
DELEGATE, CREATE, RENAME and ROOMMODE require the actual identity. A may still
rename its children using A's ordinary owner operation.

## Pure mirror

```sh
cooloo mirror 002 <B-id> <nickname>
```

B is only a credential for A's message identity. B authentication defaults to A;
message permissions equal A's, not A's administration privileges. There is no
second active inbox or sender:

- All SEND to B (including A:002) fail with forbidden, without forwarding.
- All SEND with effective sender B fail, even with explicit `--as B` or parent
  credentials. Server checks both endpoints under the append lock.
- B defaults to A for READ/FOLLOW/ROOMS/SENT/SEND on Native and SSH.
- B cannot grant, create avatars/mirrors, detach, rename, create rooms or change
  room modes as A. B cannot use its own identity to bypass this restriction.
- A and mirrors see A's readable children, including B's metadata/inbox directory.
  B's directory can be read/synchronized but receives no new messages. Existing
  history is retained. `mirror:1` in METAFILE is display data, not authority.
- Mirror permissions are fixed; `delegate B ...` is rejected. Use DETACH to revoke
  the mirror relationship. Ordinary avatars and mirrors cannot have children.

```sh
cooloo identity                 # actual credential and effective identity
cooloo whoami                  # actual credential, never the default parent
cooloo detach <B-id>            # A's actual credential; B becomes independent
```

DETACH preserves B's ID, key, nickname, history and room memberships. Existing
connections still using A lose access on the next operation/pump. B's subsequent
connections default to B and may send/receive independently. No transitive grants.

## Protocol and clients

Native commands:

```text
IDENTITY                         -> IDENTITY <authenticated-id> <effective-id>
AS <effective-id>                 -> OK
AVATAR <slot> <child-id> <nickname> -> USER ...
MIRROR <slot> <child-id> <nickname> -> USER ...
DELEGATE <child-id> read|write|read,write|none -> OK
DETACH <child-id>                 -> OK
SENT <peer-user-id> <scan-byte-offset>
```

IDENTITY works before registration; WHOAMI/HELLO are bound to actual credentials.
A mirror Native connection must query IDENTITY or explicitly AS before business
commands; unaware old clients fail rather than silently cache A's messages as B.
AS checks at least one message permission, then each operation checks its bit.
AS cannot change during READ/SENT/FOLLOW. AS failure never falls back to self.
SSH accepts matching lowercase commands and optional `--as` before the command.
Its forced-command identity remains the actual authentication context.

Updated CLI queries IDENTITY before every connection's business commands, uses
the effective ID for cache/history, and exposes `identity` (also Machine JSON).
GUI discovers the mirror default before loading views, verifies every stream's
identity and maintains separate room directories under the effective identity's
cache partition. A and B folders synchronize via bounded background subscriptions;
`.meta` is fetched for background subscriptions as well as explicitly opened rooms.
Known mirrors cannot be GUI send targets. A changed default on reconnect fails
closed until Workspace: Identity resets the views/drafts; empty selects the
server default. Lua request() uses the server default; request_as() is explicit.

## Trusted storage and reset

`$COOLOO_HOME/relations/<child-id>` contains:

```text
RELATION-1 <owner-id> <slot> <mirror:0-or-1> <parent-read/write-bits>
```

Bits 1/2 represent read/write. Files are 0600, directories 0700; invalid records
fail closed. Missing records mean independent accounts. Relation creation checks
both identities, uniqueness and no nesting. Metadata owner/slot/mirror fields are
overlaid from this trusted record; editing user metadata cannot create or alter a
relationship. Future user property APIs MUST NOT expose relations, filesystem paths,
registry operations or authentication context. Server-admin access is trusted.

Every operation and FOLLOW pump rechecks under the registry lock, including SEND
payload completion. Records are atomically replaced and fsynced; the existing
lack of complete directory-entry power-loss transactions remains. Revocation cannot
recall queued/transmitted messages or local caches. Backups must include relations.

**No legacy delegation compatibility.** Before deployment: block gate, stop daemon,
wait for all writers to exit, take a complete restricted backup, then run:

```sh
python3 tools/reset_legacy_relations.py /var/cooloo          # dry run
python3 tools/reset_legacy_relations.py /var/cooloo --apply
```

This clears old user owner/slot fields and removes obsolete authz. Accounts, keys,
nicknames, messages and room ACLs remain unchanged. The script refuses existing
new relations; it is offline, idempotent and not run automatically on startup.
Old authz is never consulted. Native/gate must be upgraded together. Do not restore
old binaries against new mirror data: they would reopen B's sending/inbox paths.

## Filtered sent history (unchanged limits)

SENT accepts user inboxes only, returns MSG for `from == effective identity`, and
ends with `END <scan-byte-offset>`. It scans past unmatched records, including
empty results. Invalid/non-boundary/beyond-end offsets fail. Physical cursors reveal
peer inbox size/activity, not unrelated message content. No server index, duplicate
body, whole-server scan or contact record is introduced.

Filtered records are NOT a contiguous inbox: never append them to the ordinary
JSONL cache. Deduplicate by (destination, source-end-offset). Log replacement or
truncation is outside the cursor model. CLI history merges inbox and SENT for peers
discovered from incoming messages, with 10,000-message/256-peer bounds and explicit
failure on overflow. Equal timestamps use destination/offset, not causal order.
GUI merges sparse outgoing records in memory, without advancing READ/FOLLOW's
checkpoint; reconnect refreshes them, not continuous cross-device sent tracking.
Peers that never replied are not discovered: known IDs may be queried with SENT.
This is not complete multi-device recovery, read-receipt or contact sync, or E2EE.

## Validation

`make -C server test`, `make -C client test`, Lua provider mocks and
`python3 client/tests/mirror_workbench.py` cover model/Native/gate parity, parent
read/write separation, mirror default/sender/inbox restrictions, administration
rejection, detach and FOLLOW revocation, metadata tampering, offline legacy reset,
filtered history and real runner/SDL-dummy dual-directory cache synchronization.
Desktop Windows/macOS GUI acceptance and production checks are separate.
