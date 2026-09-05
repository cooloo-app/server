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

#endif /* COOLOO_STORE_H */
