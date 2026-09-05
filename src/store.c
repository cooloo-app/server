/* store.c - see store.h */
#define _DEFAULT_SOURCE         /* flock(2) on glibc with -std=c99 */

#include "store.h"
#include "cooloo.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>

/* Room files live next to this binary's data; path buffer is generous. */
#define COOLOO_PATH_BUF 1024

off_t cooloo_store_append(const char *room, const char *nick,
                          const char *escaped_text)
{
    char path[COOLOO_PATH_BUF];
    char line[COOLOO_MAX_MSG + 256];
    int fd, len;
    off_t end;

    if (cooloo_room_path(path, sizeof(path), room) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    len = snprintf(line, sizeof(line), "%ld %s %s\n",
                   (long)time(NULL), nick, escaped_text);
    if (len <= 0 || (size_t)len >= sizeof(line)) {
        errno = EOVERFLOW;
        return -1;
    }

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX) < 0) {
        close(fd);
        return -1;
    }
    /* Single write() under flock + O_APPEND keeps lines interleave-free. */
    if (write(fd, line, (size_t)len) != len) {
        flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }
    end = lseek(fd, 0, SEEK_END);
    flock(fd, LOCK_UN);
    close(fd);
    return end;
}

/* Emit one stored line to stdout in read format.
 * Returns offset just past the line, or -1 if line malformed. */
static off_t emit_line(const char *line, size_t len, off_t line_start)
{
    const char *p = line, *end = line + len;
    const char *sp1, *sp2;
    off_t next = line_start + (off_t)len + 1; /* + '\n' */

    sp1 = memchr(p, ' ', (size_t)(end - p));
    if (!sp1)
        return -1;
    sp2 = memchr(sp1 + 1, ' ', (size_t)(end - sp1 - 1));
    if (!sp2)
        return -1;

    /* <end_offset> <ts> <nick> <text> */
    printf("%lld %.*s %.*s %.*s\n",
           (long long)next,
           (int)(sp1 - p), p,
           (int)(sp2 - sp1 - 1), sp1 + 1,
           (int)(end - sp2 - 1), sp2 + 1);
    return next;
}

int cooloo_store_read(const char *room, off_t offset)
{
    return cooloo_store_read_update(room, &offset);
}

int cooloo_store_read_update(const char *room, off_t *offset)
{
    char path[COOLOO_PATH_BUF];
    char line[COOLOO_MAX_MSG + 512];
    FILE *fp;
    struct stat st;
    int rc = 0;

    if (cooloo_room_path(path, sizeof(path), room) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT)
            return 0;           /* empty room: nothing to read */
        return -1;
    }
    if (fstat(fileno(fp), &st) == 0 && *offset >= st.st_size)
        goto out;               /* offset past EOF == read to tail */

    if (fseek(fp, *offset, SEEK_SET) != 0) {
        rc = -1;
        goto out;
    }
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        off_t here = ftell(fp);

        if (len == 0)
            continue;
        if (line[len - 1] != '\n') {
            /* Truncated tail (crash mid-write) or over-long line:
             * do not emit, leave it for the next read. */
            break;
        }
        if (emit_line(line, len - 1, here - (off_t)len) < 0)
            break;              /* malformed line: stop, keep offset */
        *offset = here;         /* advance past the emitted line */
    }

out:
    fclose(fp);
    return rc;
}

int cooloo_store_tail(const char *room, long n)
{
    char path[COOLOO_PATH_BUF];
    FILE *fp;
    struct stat st;
    off_t pos, start;
    char *buf = NULL;
    int rc = -1;

    if (n < 1)
        n = 1;
    if (n > COOLOO_TAIL_MAX)
        n = COOLOO_TAIL_MAX;

    if (cooloo_room_path(path, sizeof(path), room) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }
    if (fstat(fileno(fp), &st) != 0)
        goto out;
    if (st.st_size == 0) {
        rc = 0;
        goto out;
    }

    /* Scan backwards for newlines to find the start of the last n lines. */
    pos = st.st_size;
    if (pos > 0) {
        off_t scan = pos;
        long found = 0;         /* newlines seen from the end */

        /* If file ends with '\n', that newline terminates the last line;
         * skip it before counting line starts. */
        if (fseek(fp, pos - 1, SEEK_SET) == 0 && fgetc(fp) == '\n')
            scan = pos - 1;

        while (scan > 0) {
            off_t chunk = scan > 8192 ? scan - 8192 : 0;
            off_t i;

            if (fseek(fp, chunk, SEEK_SET) != 0)
                goto out;
            for (i = scan - 1; i >= chunk; i--) {
                if (fgetc(fp) == '\n') {
                    found++;
                    if (found == n) {
                        scan = i + 1;   /* start of the n-th line from end */
                        goto have_start;
                    }
                }
                /* fgetc reads sequentially; rewind one step to go backwards */
                if (i > chunk && fseek(fp, i - 1, SEEK_SET) != 0)
                    goto out;
            }
            scan = chunk;
        }
        scan = 0;               /* fewer than n lines: whole file */
have_start:
        start = scan;
    } else {
        start = 0;
    }

    (void)buf;
    if (fseek(fp, start, SEEK_SET) != 0)
        goto out;
    {
        char line[COOLOO_MAX_MSG + 512];
        while (fgets(line, sizeof(line), fp)) {
            size_t len = strlen(line);
            off_t here = ftell(fp);
            if (len == 0)
                continue;
            if (line[len - 1] != '\n')
                break;
            if (emit_line(line, len - 1, here - (off_t)len) < 0)
                break;
        }
    }
    rc = 0;

out:
    fclose(fp);
    return rc;
}
