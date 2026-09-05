/*
 * proto.c - native line protocol (handoff §4.2). See proto.h for layering.
 *
 * Notes:
 * - MSG lines carry no room field: per connection, MSG sources are
 *   serialized (one xfer at a time; live follow pushes pause while a
 *   READ/catch-up xfer is active — no loss, offsets just wait).
 * - READ/FOLLOW stream through store's emit hook; when the outbound
 *   buffer approaches capacity the hook stops the store loop without
 *   advancing the offset, and the xfer resumes from proto_on_writable.
 * - users.conf (<home>/users.conf, "<fp64> <nick>"): registration is
 *   first-come-first-served (D20); nick conflict check and append happen
 *   under a single flock hold (check-and-set atomic).
 */
#define _DEFAULT_SOURCE         /* flock(2) with -std=c99 on glibc */

#include "proto.h"
#include "store.h"
#include "escape.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

#define PROTO_MAX_USERS   4096
#define XFER_RESERVE      (8 * 1024)   /* headroom kept free in out buffer */
#define PATH_BUF          1024

/* ---------------------------------------------------------------- */
/* outbound framing                                                 */
/* ---------------------------------------------------------------- */

int proto_out(struct pconn *c, const char *line, size_t len)
{
    size_t n;
    if (c->out_len + len + NOISE_FRAME_OVERHEAD > CONN_OUT_CAP) {
        c->close_pending = 1;        /* should not happen: xfers suspend */
        return -1;
    }
    n = noise_session_send(&c->sess, (const uint8_t *)line, len,
                           c->out + c->out_len, CONN_OUT_CAP - c->out_len);
    if (!n) {
        c->close_pending = 1;
        return -1;
    }
    c->out_len += n;
    return 0;
}

static void outf(struct pconn *c, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (len <= 0 || (size_t)len >= sizeof(line)) {
        c->close_pending = 1;
        return;
    }
    proto_out(c, line, (size_t)len);
}

static void err(struct pconn *c, const char *code, const char *msg)
{
    outf(c, "ERR %s %s\n", code, msg);
}

/* ---------------------------------------------------------------- */
/* users.conf registry (D20)                                        */
/* ---------------------------------------------------------------- */

static struct {
    char fp[65];
    char nick[COOLOO_MAX_NICK + 1];
} g_users[PROTO_MAX_USERS];
static int g_nusers;

static void users_path(char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%s/users.conf", cooloo_home());
}

void proto_users_init(void)
{
    char path[PATH_BUF];
    FILE *fp;

    g_nusers = 0;
    users_path(path, sizeof(path));
    fp = fopen(path, "r");
    if (!fp)
        return;                      /* no registry yet */
    while (g_nusers < PROTO_MAX_USERS &&
           fscanf(fp, "%64s %16s", g_users[g_nusers].fp,
                  g_users[g_nusers].nick) == 2)
        g_nusers++;
    fclose(fp);
}

void proto_lookup_nick(const char *fp_hex, char out_nick[COOLOO_MAX_NICK + 1])
{
    int i;
    out_nick[0] = '\0';
    for (i = 0; i < g_nusers; i++)
        if (!strcmp(g_users[i].fp, fp_hex)) {
            memcpy(out_nick, g_users[i].nick, COOLOO_MAX_NICK + 1);
            return;
        }
}

static int nick_valid(const char *nick)
{
    size_t len, i;
    len = strlen(nick);
    if (len < 1 || len > COOLOO_MAX_NICK)
        return 0;
    for (i = 0; i < len; i++) {
        char ch = nick[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-')
            continue;
        return 0;
    }
    return 1;
}

/* copy a validated room name; callers already ran cooloo_room_valid, so
 * this is just a bounded copy (avoids gcc -Wformat-truncation noise that
 * snprintf("%s") triggers even on proven-safe paths) */
static void room_copy(char dst[COOLOO_MAX_ROOM + 1], const char *src)
{
    size_t n = strlen(src);
    if (n > COOLOO_MAX_ROOM)
        n = COOLOO_MAX_ROOM;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 0 = registered, 1 = nick-taken, -1 = io error. fp known-new (daemon
 * already looked it up), but re-check under the lock anyway (race). */
static int users_register(const char *fp_hex, const char *nick)
{
    char path[PATH_BUF];
    char line_fp[65], line_nick[COOLOO_MAX_NICK + 1];
    FILE *f;
    int fd, rc = 0;

    users_path(path, sizeof(path));
    f = fopen(path, "a+");
    if (!f)
        return -1;
    fd = fileno(f);
    if (flock(fd, LOCK_EX) < 0) {
        fclose(f);
        return -1;
    }
    /* check-and-set under the same lock hold */
    rewind(f);
    while (fscanf(f, "%64s %16s", line_fp, line_nick) == 2) {
        if (!strcmp(line_nick, nick)) {
            rc = 1;
            goto out;
        }
    }
    if (fseek(f, 0, SEEK_END) != 0 ||
        fprintf(f, "%s %s\n", fp_hex, nick) < 0 ||
        fflush(f) != 0) {
        rc = -1;
        goto out;
    }
    if (g_nusers < PROTO_MAX_USERS) {
        snprintf(g_users[g_nusers].fp, 65, "%s", fp_hex);
        snprintf(g_users[g_nusers].nick, COOLOO_MAX_NICK + 1, "%s", nick);
        g_nusers++;
    }
out:
    flock(fd, LOCK_UN);
    fclose(f);
    return rc;
}

/* ---------------------------------------------------------------- */
/* MSG streaming (READ / FOLLOW catch-up / live follow)             */
/* ---------------------------------------------------------------- */

static int emit_msg(void *ctx, long long end,
                    const char *ts, size_t ts_len,
                    const char *nick, size_t nick_len,
                    const char *text, size_t text_len)
{
    struct pconn *c = ctx;
    char hdr[96];
    int hdr_len;

    /* one frame: "MSG <end> <ts> <nick> <tlen>\n<text>\n" */
    if (c->out_len + sizeof(hdr) + ts_len + nick_len + text_len + 2 +
        NOISE_FRAME_OVERHEAD > CONN_OUT_CAP - XFER_RESERVE)
        return 1;                    /* suspend: offset stays, resume later */

    hdr_len = snprintf(hdr, sizeof(hdr), "MSG %lld %.*s %.*s %zu\n",
                       end, (int)ts_len, ts, (int)nick_len, nick, text_len);
    if (hdr_len <= 0 || (size_t)hdr_len >= sizeof(hdr)) {
        c->close_pending = 1;
        return 1;
    }
    {
        /* header + text + LF fit one frame (escaped text <= 4096) */
        uint8_t frame_pt[96 + COOLOO_MAX_MSG + 2];
        size_t n;
        memcpy(frame_pt, hdr, (size_t)hdr_len);
        memcpy(frame_pt + hdr_len, text, text_len);
        frame_pt[hdr_len + text_len] = '\n';
        n = noise_session_send(&c->sess, frame_pt,
                               (size_t)hdr_len + text_len + 1,
                               c->out + c->out_len,
                               CONN_OUT_CAP - c->out_len);
        if (!n) {
            c->close_pending = 1;
            return 1;
        }
        c->out_len += n;
    }
    return 0;
}

static void xfer_finish(struct pconn *c)
{
    if (c->xfer.is_read)
        proto_out(c, "END\n", 4);
    else if (c->xfer.follow_idx >= 0 &&
             c->xfer.follow_idx < c->nfollows)
        c->follows[c->xfer.follow_idx].live = 1;
    c->xfer.active = 0;
}

/* Run the active xfer until EOF (finish) or output pressure (suspend). */
static void xfer_pump(struct pconn *c)
{
    off_t before;

    if (!c->xfer.active)
        return;
    before = c->xfer.offset;
    cooloo_store_set_emit(emit_msg, c);
    cooloo_store_read_update(c->xfer.room, &c->xfer.offset);
    cooloo_store_set_emit(NULL, NULL);
    if (c->close_pending)
        return;
    if (c->xfer.offset == before) {
        /* emitted nothing: EOF or a truncated tail line — either way
         * the available data is exhausted */
        if (c->out_len <= CONN_OUT_CAP - XFER_RESERVE)
            xfer_finish(c);
    }
    /* progress made but hook stopped mid-way: suspended, wait for drain */
}

void proto_on_writable(struct pconn *c)
{
    if (c->xfer.active && c->out_len < CONN_OUT_LOWAT)
        xfer_pump(c);
}

/* Backward scan for the start offset of the last n lines; mirrors
 * cooloo_store_tail's scan so READ -N can share the forward xfer path
 * (and thus suspend/resume). Returns 0 on empty/missing room. */
static off_t tail_start(const char *room, long n)
{
    char path[PATH_BUF];
    char *buf = NULL;
    FILE *fp;
    struct stat st;
    off_t pos, start = 0;
    long found = 0;

    if (cooloo_room_path(path, sizeof(path), room) != 0)
        return 0;
    fp = fopen(path, "r");
    if (!fp)
        return 0;
    if (fstat(fileno(fp), &st) != 0 || st.st_size == 0)
        goto out;
    pos = st.st_size;
    buf = malloc(8192);
    if (!buf)
        goto out;
    if (pos > 0) {
        off_t scan = pos;
        if (fseek(fp, pos - 1, SEEK_SET) == 0 && fgetc(fp) == '\n')
            scan = pos - 1;
        while (scan > 0 && found < n) {
            off_t chunk = scan > 8192 ? scan - 8192 : 0;
            off_t i;
            size_t got;
            if (fseek(fp, chunk, SEEK_SET) != 0)
                break;
            got = fread(buf, 1, (size_t)(scan - chunk), fp);
            for (i = (off_t)got - 1; i >= 0; i--) {
                if (buf[i] == '\n') {
                    found++;
                    if (found == n) {
                        start = chunk + i + 1;
                        goto out;
                    }
                }
            }
            scan = chunk;
        }
        start = 0;
    }
out:
    free(buf);
    fclose(fp);
    return start;
}

void proto_tick(struct pconn *c)
{
    int i;

    if (!c->established || c->close_pending)
        return;
    if (c->xfer.active)
        xfer_pump(c);
    if (c->xfer.active)
        return;                    /* MSG serialization: follows wait */
    for (i = 0; i < c->nfollows; i++) {
        char path[PATH_BUF];
        struct stat st;

        if (!c->follows[i].live)
            continue;
        if (cooloo_room_path(path, sizeof(path), c->follows[i].room) != 0)
            continue;
        if (stat(path, &st) != 0)
            continue;
        if (st.st_size < c->follows[i].offset)
            c->follows[i].offset = 0;   /* log rotated/recreated: reset */
        if (st.st_size == c->follows[i].offset)
            continue;
        if (c->out_len > CONN_OUT_CAP - XFER_RESERVE)
            return;
        cooloo_store_set_emit(emit_msg, c);
        cooloo_store_read_update(c->follows[i].room, &c->follows[i].offset);
        cooloo_store_set_emit(NULL, NULL);
        if (c->close_pending)
            return;
    }
}

/* ---------------------------------------------------------------- */
/* commands                                                         */
/* ---------------------------------------------------------------- */

static void cmd_hello(struct pconn *c, const char *nick)
{
    int rc;
    if (c->nick[0]) {
        err(c, "already-bound", "connection already has a nick");
        return;
    }
    if (!nick_valid(nick)) {
        err(c, "nick-invalid", "want [a-z0-9_-]{1,16}");
        return;
    }
    rc = users_register(c->fp, nick);
    if (rc == 1) {
        err(c, "nick-taken", nick);
        return;
    }
    if (rc != 0) {
        err(c, "internal", "users.conf write failed");
        return;
    }
    snprintf(c->nick, sizeof(c->nick), "%s", nick);
    outf(c, "NICK %s\n", c->nick);
}

static void cmd_rooms(struct pconn *c)
{
    static char names[4096][COOLOO_MAX_ROOM + 1];
    char dir[PATH_BUF];
    DIR *dp;
    struct dirent *de;
    int count = 0, i, j;

    if (cooloo_rooms_dir(dir, sizeof(dir)) != 0) {
        err(c, "internal", "rooms path too long");
        return;
    }
    dp = opendir(dir);
    if (dp) {
        while ((de = readdir(dp)) != NULL && count < 4096) {
            const char *n = de->d_name;
            size_t len = strlen(n);
            if (len < 5 || strcmp(n + len - 4, ".log") != 0)
                continue;
            len -= 4;
            if (len < 1 || len > COOLOO_MAX_ROOM)
                continue;
            memcpy(names[count], n, len);
            names[count][len] = '\0';
            count++;
        }
        closedir(dp);
    }
    /* insertion sort is fine at this scale and needs no qsort comparator */
    for (i = 1; i < count; i++) {
        char tmp[COOLOO_MAX_ROOM + 1];
        memcpy(tmp, names[i], sizeof tmp);
        for (j = i; j > 0 && strcmp(names[j - 1], tmp) > 0; j--)
            memcpy(names[j], names[j - 1], sizeof names[0]);
        memcpy(names[j], tmp, sizeof names[0]);
    }
    for (i = 0; i < count; i++)
        outf(c, "ROOM %s\n", names[i]);
    proto_out(c, "END\n", 4);
}

static void cmd_send(struct pconn *c, const char *room, const char *len_s)
{
    unsigned long len;
    char *endp = NULL;

    if (!cooloo_room_valid(room)) {
        err(c, "room-invalid", room);
        return;
    }
    errno = 0;
    len = strtoul(len_s, &endp, 10);
    if (errno || !endp || *endp || len == 0 || len > 1024 * 1024) {
        err(c, "bad-request", "SEND <room> <len 1..1048576>");
        return;
    }
    c->sendst.active = 1;
    c->sendst.got = 0;
    c->sendst.len = (size_t)len;
    room_copy(c->sendst.room, room);
    if (len > SEND_RAW_MAX) {
        /* already answered; just drain the payload off the stream */
        c->sendst.discard = 1;
        err(c, "message-too-large", "max 8192 raw bytes");
    } else {
        c->sendst.discard = 0;
    }
}

static void sendst_consume(struct pconn *c)
{
    size_t avail = c->in_len;
    size_t need = c->sendst.len - c->sendst.got;
    size_t take = avail < need ? avail : need;

    if (!c->sendst.discard)
        memcpy(c->sendst.buf + c->sendst.got, c->in, take);
    c->sendst.got += take;
    memmove(c->in, c->in + take, c->in_len - take);
    c->in_len -= take;
    if (c->sendst.got < c->sendst.len)
        return;

    if (!c->sendst.discard) {
        char esc[COOLOO_MAX_MSG + 1];
        long esclen = cooloo_escape(esc, sizeof(esc),
                                    (const char *)c->sendst.buf,
                                    c->sendst.len);
        if (esclen < 0) {
            err(c, "message-too-large", "max 4096 escaped bytes");
        } else {
            off_t end = cooloo_store_append(c->sendst.room, c->nick, esc);
            if (end < 0)
                err(c, "internal", "write failed");
            else
                outf(c, "OK %lld\n", (long long)end);
        }
    }
    c->sendst.active = 0;
}

static void cmd_read(struct pconn *c, const char *room, const char *arg)
{
    off_t offset;

    if (!cooloo_room_valid(room)) {
        err(c, "room-invalid", room);
        return;
    }
    if (c->xfer.active) {
        err(c, "bad-request", "transfer in progress");
        return;
    }
    if (arg[0] == '-') {
        long n = strtol(arg + 1, NULL, 10);
        if (n < 1 || n > COOLOO_TAIL_MAX) {
            err(c, "bad-request", "READ -N wants 1..1000");
            return;
        }
        offset = tail_start(room, n);
    } else {
        char *endp = NULL;
        errno = 0;
        offset = (off_t)strtoll(arg, &endp, 10);
        if (errno || !endp || *endp || offset < 0) {
            err(c, "bad-request", "READ <room> <offset|-N>");
            return;
        }
    }
    c->xfer.active = 1;
    c->xfer.is_read = 1;
    c->xfer.follow_idx = -1;
    c->xfer.offset = offset;
    room_copy(c->xfer.room, room);
    xfer_pump(c);
}

static void cmd_follow(struct pconn *c, const char *room, const char *arg)
{
    int idx;

    if (!cooloo_room_valid(room)) {
        err(c, "room-invalid", room);
        return;
    }
    if (c->nfollows >= CONN_MAX_FOLLOWS) {
        err(c, "bad-request", "too many follows (max 8)");
        return;
    }
    idx = c->nfollows++;
    room_copy(c->follows[idx].room, room);
    c->follows[idx].live = 0;

    if (arg) {
        char *endp = NULL;
        off_t offset;
        errno = 0;
        offset = (off_t)strtoll(arg, &endp, 10);
        if (errno || !endp || *endp || offset < 0) {
            c->nfollows--;
            err(c, "bad-request", "FOLLOW <room> [offset]");
            return;
        }
        if (c->xfer.active) {
            c->nfollows--;
            err(c, "bad-request", "transfer in progress");
            return;
        }
        c->follows[idx].offset = offset;
        c->xfer.active = 1;
        c->xfer.is_read = 0;
        c->xfer.follow_idx = idx;
        c->xfer.offset = offset;
        room_copy(c->xfer.room, room);
        xfer_pump(c);
    } else {
        /* from tail: live immediately at current size */
        char path[PATH_BUF];
        struct stat st;
        off_t size = 0;
        if (cooloo_room_path(path, sizeof(path), room) == 0 &&
            stat(path, &st) == 0)
            size = st.st_size;
        c->follows[idx].offset = size;
        c->follows[idx].live = 1;
    }
}

static void exec_line(struct pconn *c, char *line)
{
    char cmd[16], a1[64], a2[64];
    int n;

    c->last_active = time(NULL);
    n = sscanf(line, "%15s %63s %63s", cmd, a1, a2);
    if (n < 1)
        return;                    /* blank line: ignore */

    /* D25: unregistered keys may only HELLO/WHOAMI/PING/ROOMS */
    if (!c->nick[0] && strcmp(cmd, "HELLO") && strcmp(cmd, "WHOAMI") &&
        strcmp(cmd, "PING") && strcmp(cmd, "ROOMS")) {
        err(c, "not-registered", "register first: HELLO <nick>");
        return;
    }

    if (!strcmp(cmd, "PING")) {
        proto_out(c, "PONG\n", 5);
    } else if (!strcmp(cmd, "WHOAMI")) {
        outf(c, "NICK %s\n", c->nick[0] ? c->nick : "-");
    } else if (!strcmp(cmd, "ROOMS")) {
        cmd_rooms(c);
    } else if (!strcmp(cmd, "HELLO")) {
        if (n < 2)
            err(c, "bad-request", "HELLO <nick>");
        else
            cmd_hello(c, a1);
    } else if (!strcmp(cmd, "SEND")) {
        if (n < 3)
            err(c, "bad-request", "SEND <room> <len>");
        else
            cmd_send(c, a1, a2);
    } else if (!strcmp(cmd, "READ")) {
        if (n < 3)
            err(c, "bad-request", "READ <room> <offset|-N>");
        else
            cmd_read(c, a1, a2);
    } else if (!strcmp(cmd, "FOLLOW")) {
        if (n < 2)
            err(c, "bad-request", "FOLLOW <room> [offset]");
        else
            cmd_follow(c, a1, n >= 3 ? a2 : NULL);
    } else {
        err(c, "unknown-command", cmd);
    }
}

/* ---------------------------------------------------------------- */
/* entry points                                                     */
/* ---------------------------------------------------------------- */

void proto_on_established(struct pconn *c)
{
    outf(c, "COOLOO 1 %s\n", c->nick[0] ? c->nick : "-");
}

void proto_on_plain(struct pconn *c, const uint8_t *pt, size_t len)
{
    if (len > CONN_IN_CAP - c->in_len) {
        c->close_pending = 1;      /* command stream flooded the buffer */
        return;
    }
    memcpy(c->in + c->in_len, pt, len);
    c->in_len += len;

    for (;;) {
        char *nl;
        size_t line_len;

        if (c->sendst.active) {
            sendst_consume(c);
            if (c->sendst.active)
                return;            /* payload incomplete */
            continue;
        }
        nl = memchr(c->in, '\n', c->in_len);
        if (!nl) {
            if (c->in_len >= CONN_IN_CAP)
                c->close_pending = 1;
            return;
        }
        line_len = (size_t)(nl - (char *)c->in);
        if (line_len > 0 && c->in[line_len - 1] == '\r')
            c->in[line_len - 1] = '\0';
        else
            c->in[line_len] = '\0';
        exec_line(c, (char *)c->in);
        if (c->close_pending)
            return;
        memmove(c->in, nl + 1, c->in_len - line_len - 1);
        c->in_len -= line_len + 1;
    }
}
