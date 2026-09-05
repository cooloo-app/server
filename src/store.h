/* store.h - append-only room log storage
 *
 * Layout: $COOLOO_HOME/rooms/<room>.log
 * Each line: <unix_ts> <nick> <escaped_text>\n
 *
 * Concurrency (D13): per-room flock + O_APPEND + single write().
 * Offsets are byte offsets into the log file.
 */
#ifndef COOLOO_STORE_H
#define COOLOO_STORE_H

#include <sys/types.h>

/* Append one message to room log. Creates the room file if missing (D12).
 * On success returns end offset (file size after write), or -1 on error
 * (errno set). Caller must have validated room/text already. */
off_t cooloo_store_append(const char *room, const char *nick,
                          const char *escaped_text);

/* Read from byte offset, printing lines to stdout as:
 *   <end_offset> <ts> <nick> <escaped_text>
 * A truncated tail line (crash mid-write) is not emitted and not
 * counted into offsets. Returns 0 on success, -1 on error. */
int cooloo_store_read(const char *room, off_t offset);

/* Read the last n lines (n >= 1), same output format as read.
 * Returns 0 on success, -1 on error. */
int cooloo_store_tail(const char *room, long n);

/* Incremental read for follow(): emit complete lines starting at *offset
 * and advance *offset past the last complete line emitted. Truncated
 * tail data is left for the next call. Returns 0 on success, -1 on error. */
int cooloo_store_read_update(const char *room, off_t *offset);

/* Emit hook (v1 daemon): when set, read/tail/read_update deliver each
 * parsed line through fn(ctx, ...) instead of printf to stdout.
 * ts/nick/text are NOT NUL-terminated; use the lengths.
 * fn returns 0 to continue; non-zero stops the read loop WITHOUT
 * advancing past the current line (daemon uses this to suspend output
 * when the connection buffer is full, resuming from the same offset).
 * Single-threaded use only; the v0 gate never sets this (NULL = stdout). */
typedef int (*cooloo_emit_fn)(void *ctx, long long end_offset,
                              const char *ts, size_t ts_len,
                              const char *nick, size_t nick_len,
                              const char *text, size_t text_len);
void cooloo_store_set_emit(cooloo_emit_fn fn, void *ctx);

#endif /* COOLOO_STORE_H */
