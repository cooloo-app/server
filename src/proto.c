/* Native protocol 2: authenticated public IDs and canonical JSONL. */
#define _POSIX_C_SOURCE 200809L
#include "proto.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

static model db;
static void file_pump(struct pconn *c);

int proto_out(struct pconn *c, const char *line, size_t len)
{
    size_t n;
    if (len + NOISE_FRAME_OVERHEAD > CONN_OUT_CAP - c->out_len) return -1;
    n = noise_session_send(&c->sess, (const uint8_t *)line, len,
                           c->out + c->out_len, CONN_OUT_CAP - c->out_len);
    if (!n) { c->close_pending = 1; return -1; }
    c->out_len += n;
    return 0;
}
static void outf(struct pconn *c, const char *fmt, ...)
{
    char s[1024]; int n; va_list ap;
    va_start(ap, fmt); n = vsnprintf(s, sizeof s, fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= sizeof s || proto_out(c, s, (size_t)n)) c->close_pending = 1;
}
static void error(struct pconn *c)
{
    int e = errno;
    outf(c, "ERR %s %s\n", e == EACCES ? "forbidden" : e == ENOENT ? "not-found" :
         e == EEXIST ? "exists" : e == ESTALE ? "conflict" : e == EINVAL ? "bad-request" :
         e == EAGAIN ? "rate-limited" : e == EALREADY ? "already-member" :
         e == ENOSPC ? "limit" : "internal", strerror(e));
}
void proto_users_init(void)
{
    if (model_open(&db, cooloo_home())) { perror("model_open"); exit(1); }
}
void proto_lookup_nick(const char *id, char out[COOLOO_MAX_NICK + 1])
{
    model_meta v;
    out[0] = 0;
    if (!model_get(&db, id, &v) && !strcmp(v.type, "user")) strcpy(out, v.nickname);
}
static int emit(void *ctx, off_t end, const char *json, size_t len)
{
    struct pconn *c = ctx;
    char record[MODEL_JSON_MAX + 100];
    int n;
    if (end <= c->xfer.skip_until && c->xfer.active) return 0;
    if (c->out_len + len + 128 > CONN_OUT_CAP - 8192) return 1;
    n = snprintf(record, sizeof record, "MSG %lld %zu\n", (long long)end, len - 1);
    memcpy(record + n, json, len);
    {
        size_t total = (size_t)n + len, sent = 0;
        while (sent < total) {
            size_t chunk = total - sent;
            if (chunk > NOISE_FRAME_MAX_PT) chunk = NOISE_FRAME_MAX_PT;
            if (proto_out(c, record + sent, chunk)) return -1;
            sent += chunk;
        }
    }
    return 0;
}
static void pump(struct pconn *c)
{
    off_t before;
    int rc;
    if (!c->xfer.active) return;
    before = c->xfer.offset;
    model_authenticate(&db,c->fp);
    rc = c->xfer.is_sent ? model_sent(&db,c->effective,c->xfer.room,&c->xfer.offset,emit,c)
                        : model_read(&db,c->effective,c->xfer.room,&c->xfer.offset,emit,c);
    if (rc) { error(c); c->xfer.active = 0; return; }
    if (c->xfer.offset == before && c->out_len < CONN_OUT_CAP - MODEL_JSON_MAX - 8320) {
        if (c->xfer.is_sent) outf(c,"END %lld\n",(long long)c->xfer.offset);
        else if (c->xfer.is_read) outf(c, "END\n");
        else {
            int i = c->xfer.follow_idx;
            c->follows[i].offset = c->xfer.offset;
            c->follows[i].live = 1;
        }
        c->xfer.active = 0;
    }
}
void proto_on_writable(struct pconn *c)
{ if (c->out_len < CONN_OUT_LOWAT) {file_pump(c);pump(c);} }
void proto_tick(struct pconn *c)
{
    int i;
    if (!c->established || c->close_pending) return;
    model_authenticate(&db,c->fp);
    file_pump(c);
    pump(c);
    if (c->xfer.active) return;
    for (i = 0; i < c->nfollows; i++) {
        if (!c->follows[i].live) continue;
        model_authenticate(&db,c->fp);
        if (model_read(&db, c->effective, c->follows[i].room, &c->follows[i].offset, emit, c)) {
            error(c); c->follows[i].live = 0;
        }
    }
}
static int room_line(void *ctx, const model_meta *v)
{ struct pconn *c = ctx; outf(c, "ROOM %s %s\n", v->id, v->nickname); return c->close_pending ? -1 : 0; }
static int boundary(void *ctx, off_t end, const char *json, size_t len)
{ (void)json; (void)len; *(off_t *)ctx = end; return 0; }
static int number(const char *s, off_t *value)
{
    char *end; long long n;
    if (!s || !*s || *s < '0' || *s > '9') return -1;
    errno = 0; n = strtoll(s, &end, 10);
    if (errno || *end || n < 0 || (long long)(off_t)n != n) return -1;
    *value = (off_t)n; return 0;
}
/* Space-delimited identifiers; remaining nickname text is not tokenized. */
static char *word(char **rest)
{
    char *start = *rest, *p;
    while (*start == ' ') start++;
    if (!*start) { *rest = start; return NULL; }
    p = strchr(start, ' ');
    if (p) { *p++ = 0; while (*p == ' ') p++; *rest = p; }
    else *rest = start + strlen(start);
    return start;
}
static int unhex(char c)
{ return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1; }
#include "file_proto.inc"
static void exec_line(struct pconn *c, char *line)
{
    char *rest = line, *cmd = word(&rest), *a, *b;
    model_meta v;
    off_t off;
    if (!cmd) return;
    model_authenticate(&db,c->fp);
    if (!strcmp(cmd, "PING")) { outf(c, "PONG\n"); return; }
    if (!strcmp(cmd, "IDENTITY")) {
        c->identity_confirmed=1;
        outf(c,"IDENTITY %s %s\n",c->fp,c->effective); return;
    }
    if (!strcmp(cmd, "WHOAMI")) {
        proto_lookup_nick(c->fp, c->nick);
        outf(c, "NICK %s\n", c->nick[0] ? c->nick : "-"); return;
    }
    if (!strcmp(cmd, "HELLO")) {
        if (model_register(&db, c->fp, rest)) error(c);
        else { strcpy(c->nick, rest); outf(c, "NICK %s\n", rest); }
        return;
    }
    if (!c->nick[0]) { outf(c, "ERR not-registered HELLO first\n"); return; }
    if (!strcmp(cmd,"AS")) {
        if(c->file_upload || c->file_output || c->xfer.active || c->nfollows) { outf(c,"ERR busy identity locked by streams\n"); return; }
        if(!model_id_valid(rest) || model_authorize(&db,rest,MODEL_READ|MODEL_WRITE)) { errno=EACCES; error(c); return; }
        strcpy(c->effective,rest); c->identity_confirmed=1; outf(c,"OK\n"); return;
    }
    if(!c->identity_confirmed) {
        outf(c,"ERR identity-required IDENTITY or explicit AS first\n");
        if(!strcmp(cmd,"SEND"))c->close_pending=1;
        return;
    }
    if(c->file_output){outf(c,"ERR busy file output active\n");return;}
    if(!strncmp(cmd,"F",1)&&strcmp(cmd,"FOLLOW")){
        if(c->nfollows||c->xfer.active){outf(c,"ERR busy message stream active\n");return;}
        if(file_command(c,cmd,rest))return;
    }
    if(c->file_upload){outf(c,"ERR busy upload active\n");return;}
    if (!strcmp(cmd, "ROOMS")) {
        if (model_list(&db, c->effective, room_line, c)) error(c);
        else outf(c, "END\n");
        return;
    }
    if (!strcmp(cmd, "CREATE")) {
        char id[65];
        if (model_room(&db, c->effective, rest, id)) error(c);
        else outf(c, "ROOM %s %s\n", id, rest);
        return;
    }
    a = word(&rest);
    if (!a) { outf(c, "ERR bad-request missing argument\n"); return; }
    if (!strcmp(cmd,"DELEGATE")) {
        unsigned bits;
        if(!strcmp(rest,"read")) bits=MODEL_READ;
        else if(!strcmp(rest,"write")) bits=MODEL_WRITE;
        else if(!strcmp(rest,"read,write")) bits=MODEL_READ|MODEL_WRITE;
        else if(!strcmp(rest,"none")) bits=0;
        else { errno=EINVAL; error(c); return; }
        if(model_delegate(&db,c->effective,a,bits)) error(c); else outf(c,"OK\n");
        return;
    }
    if (!strcmp(cmd, "ROOMMODE")) {
        if (model_room_mode(&db,c->effective,a,rest)) error(c);
        else outf(c,"OK\n");
        return;
    }
    if (!strcmp(cmd,"DETACH")) {
        if(*rest) { errno=EINVAL; error(c); return; }
        if(model_detach(&db,c->effective,a))error(c);else outf(c,"OK\n");
        return;
    }
    if (!strcmp(cmd, "AVATAR") || !strcmp(cmd,"MIRROR")) {
        b = word(&rest);
        if (number(a, &off) || off < 1 || off > 899 || !b) { outf(c, "ERR bad-request avatar arguments\n"); return; }
        if ((!strcmp(cmd,"MIRROR") ? model_mirror : model_avatar)(&db, c->effective, (unsigned)off, b, rest)) error(c);
        else outf(c, "USER %s %s\n", b, rest);
        return;
    }
    if (!strcmp(cmd, "RENAME")) {
        if (model_rename(&db, c->effective, a, rest)) error(c);
        else outf(c, "OK\n");
        return;
    }
    if (model_get(&db, a, &v)) {
        error(c);
        if (!strcmp(cmd, "SEND")) c->close_pending = 1;
        return;
    }
    if (!strcmp(cmd,"STREAM")) {
        char generation[65];
        if(*rest){errno=EINVAL;error(c);return;}
        if(model_generation(&db,c->effective,a,generation))error(c);
        else outf(c,"STREAM %s %s\n",v.id,generation);
        return;
    }
    if (!strcmp(cmd, "METAFILE")) {
        char json[1024]; int n;
        if (model_get_visible(&db,c->effective,a,&v)) { error(c); return; }
        n = model_meta_json(&v, json, sizeof json);
        if (n < 0) error(c);
        else { outf(c, "META %s", json); }
        return;
    }
    if (!strcmp(cmd, "META")) {
        if (model_get_visible(&db,c->effective,a,&v)) { error(c); return; }
        outf(c, "INFO %s %s %s %03u %s\n", v.id, v.type,
             v.owner[0] ? v.owner : "-", v.slot, v.nickname); return;
    }
    if (!strcmp(cmd, "SEND")) {
        char *length=strchr(rest,' ');
        if(length)*length++=0;
        if (!length || !model_uuid_valid(rest) || number(length, &off) || !off || off > MODEL_CONTENT_MAX) {
            outf(c, "ERR bad-request SEND length 1..4096\n");
            /* Unknown/invalid payload length cannot be safely resynchronized. */
            c->close_pending = 1; return;
        }
        c->sendst.active = 1; c->sendst.len = (size_t)off; c->sendst.got = 0;
        strcpy(c->sendst.uuid,rest);
        strcpy(c->sendst.room, v.id); return;
    }
    if (strcmp(cmd, "READ") && strcmp(cmd, "FOLLOW") && strcmp(cmd,"SENT")) {
        outf(c, "ERR unknown-command %s\n", cmd); return;
    }
    if (strcmp(cmd,"SENT") && model_can_read(&db, c->effective, v.id)) { error(c); return; }
    if (c->xfer.active) { outf(c, "ERR busy transfer active\n"); return; }
    /* Full history and explicit offsets are canonical. Legacy -N is removed. */
    if (!*rest && !strcmp(cmd, "FOLLOW")) {
        off_t cursor = 0; off = 0;
        if (model_read(&db, c->effective, v.id, &cursor, boundary, &off)) { error(c); return; }
    } else if (number(rest, &off)) { outf(c, "ERR bad-request nonnegative offset required\n"); return; }
    if (!strcmp(cmd, "FOLLOW") && c->nfollows >= CONN_MAX_FOLLOWS) {
        outf(c, "ERR limit too many follows\n"); return;
    }
    c->xfer.active = 1; c->xfer.is_read = strcmp(cmd,"FOLLOW")!=0;
    c->xfer.is_sent = !strcmp(cmd,"SENT");
    c->xfer.offset = off; c->xfer.skip_until = 0; strcpy(c->xfer.room, v.id);
    if (!c->xfer.is_read) {
        int i = c->nfollows++;
        strcpy(c->follows[i].room, v.id); c->follows[i].live = 0;
        c->xfer.follow_idx = i;
    }
    pump(c);
}
void proto_on_established(struct pconn *c)
{
    if(model_default_identity(&db,c->fp,c->effective)){c->close_pending=1;return;}
    c->identity_confirmed=!strcmp(c->effective,c->fp);
    outf(c, "COOLOO 3 %s\n", c->nick[0] ? c->nick : "-");
}
void proto_on_plain(struct pconn *c, const uint8_t *pt, size_t len)
{
    if (len > CONN_IN_CAP - c->in_len) { c->close_pending = 1; return; }
    memcpy(c->in + c->in_len, pt, len); c->in_len += len;
    for (;;) {
        if (c->sendst.active) {
            size_t n = c->sendst.len - c->sendst.got;
            off_t end;long long ts;
            if (n > c->in_len) n = c->in_len;
            memcpy(c->sendst.buf + c->sendst.got, c->in, n);
            c->sendst.got += n; memmove(c->in, c->in + n, c->in_len - n); c->in_len -= n;
            if (c->sendst.got < c->sendst.len) return;
            c->sendst.active = 0;
            model_authenticate(&db,c->fp);
            if (model_send_id(&db,c->effective,c->sendst.room,c->sendst.uuid,(char *)c->sendst.buf,c->sendst.len,&end,&ts)) {
                if(errno==EEXIST)outf(c,"ERR id-conflict message ID conflict\n");
                else if(errno==EALREADY)outf(c,"ERR duplicate-content duplicate or already applied control\n");
                else error(c);
            } else outf(c,"OK %lld %s %lld\n",(long long)end,c->sendst.uuid,ts);
        } else {
            char *nl = memchr(c->in, '\n', c->in_len);
            size_t n;
            if (!nl) { if (c->in_len > 9000) c->close_pending = 1; return; }
            n = (size_t)(nl - (char *)c->in);
            if (n > 9000 || memchr(c->in, 0, n) || memchr(c->in, '\r', n)) { c->close_pending = 1; return; }
            c->in[n] = 0; exec_line(c, (char *)c->in);
            memmove(c->in, nl + 1, c->in_len - n - 1); c->in_len -= n + 1;
        }
        if (c->close_pending || !c->in_len) return;
    }
}
