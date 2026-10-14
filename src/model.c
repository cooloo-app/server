#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <dirent.h>
#include <time.h>

static int fail(int e) { errno = e; return -1; }
int model_id_valid(const char *id)
{
    size_t i;
    if (!id || strlen(id) != 64) return 0;
    for (i = 0; i < 64; i++)
        if (!((id[i] >= '0' && id[i] <= '9') ||
              (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    return 1;
}
static int path(model *m, char *p, size_t cap, const char *id, const char *file)
{
    int n;
    if (!model_id_valid(id)) return fail(EINVAL);
    n = snprintf(p, cap, "%s/rooms/%s/%s", m->root, id, file);
    return n > 0 && (size_t)n < cap ? 0 : fail(ENAMETOOLONG);
}
static int directory(const char *p)
{
    struct stat st;
    if (mkdir(p, 0700) == 0) return 0;
    if (errno != EEXIST) return -1;
    if (lstat(p, &st) != 0) return -1;
    return S_ISDIR(st.st_mode) ? 0 : fail(ENOTDIR);
}
int model_open(model *m, const char *root)
{
    char p[1200];
    memset(m, 0, sizeof *m);
    m->lock_fd = -1;
    if (!root || !root[0] || strlen(root) >= sizeof m->root) return fail(EINVAL);
    strcpy(m->root, root);
    if (directory(root)) return -1;
    snprintf(p, sizeof p, "%s/rooms", root);
    if (directory(p)) return -1;
    snprintf(p, sizeof p, "%s/.registry.lock", root);
    m->lock_fd = open(p, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    return m->lock_fd < 0 ? -1 : 0;
}
void model_close(model *m)
{
    if (m->lock_fd >= 0) close(m->lock_fd);
    m->lock_fd = -1;
}
static int utf8(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned c = (unsigned char)s[i++], v, min, k;
        if (c < 128) { if (!c) return 0; continue; }
        if (c >= 0xc2 && c <= 0xdf) { k = 1; v = c & 31; min = 128; }
        else if (c >= 0xe0 && c <= 0xef) { k = 2; v = c & 15; min = 2048; }
        else if (c >= 0xf0 && c <= 0xf4) { k = 3; v = c & 7; min = 65536; }
        else return 0;
        if (k > n - i) return 0;
        while (k--) {
            c = (unsigned char)s[i++];
            if ((c & 192) != 128) return 0;
            v = (v << 6) | (c & 63);
        }
        if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return 0;
    }
    return 1;
}
static int nickname_valid(const char *s)
{
    size_t i, n = s ? strlen(s) : 0;
    if (!n || n > MODEL_NICK_MAX || !strcmp(s, "-") || !utf8(s, n)) return 0;
    for (i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || s[i] == 127) return 0;
    return 1;
}
/* Canonical serializer: control characters always use lowercase \\u00xx.
 * Capacity is supplied by callers at six bytes per input byte plus quotes. */
static size_t quote(char *dst, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i, w = 0;
    dst[w++] = '"';
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32) {
            memcpy(dst + w, "\\u00", 4); w += 4;
            dst[w++] = hex[c >> 4]; dst[w++] = hex[c & 15];
        } else {
            if (c == '"' || c == '\\') dst[w++] = '\\';
            dst[w++] = (char)c;
        }
    }
    dst[w++] = '"'; dst[w] = 0;
    return w;
}
static int allwrite(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        s += w; n -= (size_t)w;
    }
    return 0;
}
int model_meta_json(const model_meta *v, char *json, size_t cap)
{
    char nick[MODEL_NICK_MAX * 6 + 3]; int n;
    quote(nick, v->nickname, strlen(v->nickname));
    n = snprintf(json, cap,
        "{\"id\":\"%s\",\"type\":\"%s\",\"nickname\":%s,\"owner\":\"%s\",\"slot\":%u}\n",
        v->id, v->type, nick, v->owner, v->slot);
    return n < 0 || (size_t)n >= cap ? fail(EOVERFLOW) : n;
}
static int save_meta(model *m, const model_meta *v)
{
    char p[1200], tmp[1200], json[1024];
    int fd, n, rc = -1, e;
    if (path(m, p, sizeof p, v->id, ".meta") ||
        path(m, tmp, sizeof tmp, v->id, ".meta.tmp")) return -1;
    n = model_meta_json(v, json, sizeof json);
    if (n < 0 || (size_t)n >= sizeof json) return fail(EOVERFLOW);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    if (!allwrite(fd, json, (size_t)n) && !fsync(fd)) rc = 0;
    e = errno;
    if (close(fd) && !rc) { rc = -1; e = errno; }
    if (!rc && rename(tmp, p)) { rc = -1; e = errno; }
    if (rc) unlink(tmp);
    errno = e;
    return rc;
}
/* Metadata accepts the canonical schema only. Never eval/execute a file.
 * Names can contain escaped quotes and backslashes; controls are forbidden. */
static int load_meta(model *m, const char *id, model_meta *v)
{
    char p[1200], buf[1024], *s, *q;
    int fd, pos = 0, used = 0;
    ssize_t n;
    size_t w = 0;
    if (path(m, p, sizeof p, id, ".meta")) return -1;
    fd = open(p, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0 || n == sizeof buf - 1) return fail(EINVAL);
    buf[n] = 0;
    memset(v, 0, sizeof *v);
    if (sscanf(buf, "{\"id\":\"%64[0-9a-f]\",\"type\":\"%4[a-z]\",\"nickname\":\"%n",
               v->id, v->type, &pos) != 2 || !pos || strcmp(v->id, id)) return fail(EINVAL);
    s = buf + pos;
    while (*s && *s != '"') {
        char c = *s++;
        if (c == '\\') { c = *s++; if (c != '"' && c != '\\') return fail(EINVAL); }
        if (w >= MODEL_NICK_MAX) return fail(EINVAL);
        v->nickname[w++] = c;
    }
    if (*s++ != '"') return fail(EINVAL);
    q = s;
    if (strncmp(q, ",\"owner\":\"", 10)) return fail(EINVAL);
    q += 10;
    s = strchr(q, '"');
    if (!s || (s - q != 0 && s - q != 64)) return fail(EINVAL);
    memcpy(v->owner, q, (size_t)(s - q));
    if (sscanf(s, "\",\"slot\":%u}\n%n", &v->slot, &used) != 1 || !used || s[used]) return fail(EINVAL);
    if (!nickname_valid(v->nickname) ||
        (strcmp(v->type, "user") && strcmp(v->type, "room")) ||
        (v->owner[0] && !model_id_valid(v->owner)) || v->slot > 899) return fail(EINVAL);
    return 0;
}
static int get(model *m, const char *address, model_meta *out)
{
    char owner[65], dir[1200];
    unsigned slot;
    DIR *dp;
    struct dirent *de;
    model_meta parent;
    if (!address) return fail(EINVAL);
    if (strlen(address) == 64) return load_meta(m, address, out);
    if (strlen(address) != 68 || address[64] != ':' ||
        address[65] < '0' || address[65] > '8' ||
        address[66] < '0' || address[66] > '9' ||
        address[67] < '0' || address[67] > '9') return fail(EINVAL);
    memcpy(owner, address, 64); owner[64] = 0;
    if (load_meta(m, owner, &parent)) return -1;
    if (strcmp(parent.type, "user") || parent.owner[0]) return fail(EINVAL);
    slot = (unsigned)atoi(address + 65);
    if (!slot) { *out = parent; return 0; }
    snprintf(dir, sizeof dir, "%s/rooms", m->root);
    dp = opendir(dir);
    if (!dp) return -1;
    while ((de = readdir(dp))) {
        if (!model_id_valid(de->d_name)) continue;
        if (load_meta(m, de->d_name, out)) { int e = errno; closedir(dp); return fail(e); }
        if (!strcmp(out->owner, owner) && out->slot == slot) { closedir(dp); return 0; }
    }
    closedir(dp);
    return fail(ENOENT);
}
int model_get(model *m, const char *address, model_meta *out)
{
    int rc, e;
    if (flock(m->lock_fd, LOCK_SH)) return -1;
    rc = get(m, address, out); e = errno;
    flock(m->lock_fd, LOCK_UN); errno = e;
    return rc;
}
static int actor_user(model *m, const char *actor)
{
    model_meta a;
    if (load_meta(m, actor, &a)) return -1;
    return !strcmp(a.type, "user") ? 0 : fail(EACCES);
}
static int create(model *m, const model_meta *v)
{
    char p[1200];
    int e;
    if (path(m, p, sizeof p, v->id, "")) return -1;
    if (mkdir(p, 0700)) return -1;
    if (!save_meta(m, v)) return 0;
    e = errno; rmdir(p); errno = e; return -1;
}
int model_register(model *m, const char *actor, const char *nickname)
{
    model_meta v;
    int rc, e;
    if (!model_id_valid(actor) || !nickname_valid(nickname)) return fail(EINVAL);
    memset(&v, 0, sizeof v);
    strcpy(v.id, actor); strcpy(v.type, "user"); strcpy(v.nickname, nickname);
    if (flock(m->lock_fd, LOCK_EX)) return -1;
    rc = create(m, &v); e = errno;
    flock(m->lock_fd, LOCK_UN); errno = e;
    return rc;
}
int model_avatar(model *m, const char *actor, unsigned slot,
                 const char *child, const char *nickname)
{
    model_meta a, v, existing;
    char alias[70];
    int rc = -1, e;
    if (!model_id_valid(actor) || !model_id_valid(child) || !nickname_valid(nickname) ||
        !slot || slot > 899 || !strcmp(actor, child)) return fail(EINVAL);
    if (flock(m->lock_fd, LOCK_EX)) return -1;
    if (load_meta(m, actor, &a)) goto out;
    if (strcmp(a.type, "user") || a.owner[0]) { errno = EACCES; goto out; }
    snprintf(alias, sizeof alias, "%s:%03u", actor, slot);
    if (!get(m, alias, &existing)) { errno = EEXIST; goto out; }
    if (errno != ENOENT) goto out;
    memset(&v, 0, sizeof v);
    strcpy(v.id, child); strcpy(v.type, "user"); strcpy(v.owner, actor);
    strcpy(v.nickname, nickname); v.slot = slot;
    rc = create(m, &v);
out:
    e = errno; flock(m->lock_fd, LOCK_UN); errno = e; return rc;
}
int model_room(model *m, const char *actor, const char *nickname, char id[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char random[32];
    model_meta v;
    int fd, rc = -1, e, i;
    if (!model_id_valid(actor) || !nickname_valid(nickname)) return fail(EINVAL);
    if (flock(m->lock_fd, LOCK_EX)) return -1;
    if (actor_user(m, actor)) goto out;
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) goto out;
    if (read(fd, random, sizeof random) != sizeof random) { close(fd); errno = EIO; goto out; }
    close(fd);
    memset(&v, 0, sizeof v);
    for (i = 0; i < 32; i++) { v.id[i*2] = hex[random[i] >> 4]; v.id[i*2+1] = hex[random[i] & 15]; }
    strcpy(v.type, "room"); strcpy(v.nickname, nickname); strcpy(v.owner, actor);
    rc = create(m, &v);
    if (!rc) strcpy(id, v.id);
out:
    e = errno; flock(m->lock_fd, LOCK_UN); errno = e; return rc;
}
int model_rename(model *m, const char *actor, const char *address, const char *nickname)
{
    model_meta v;
    int rc = -1, e;
    if (!nickname_valid(nickname)) return fail(EINVAL);
    if (flock(m->lock_fd, LOCK_EX)) return -1;
    if (actor_user(m, actor) || get(m, address, &v)) goto out;
    if (strcmp(actor, v.id) && strcmp(actor, v.owner)) { errno = EACCES; goto out; }
    strcpy(v.nickname, nickname); rc = save_meta(m, &v);
out:
    e = errno; flock(m->lock_fd, LOCK_UN); errno = e; return rc;
}
static int readable(model *m, const char *actor, const char *address, model_meta *v)
{
    if (actor_user(m, actor) || get(m, address, v)) return -1;
    return !strcmp(v->type, "room") || !strcmp(actor, v->id) ? 0 : fail(EACCES);
}
int model_can_read(model *m, const char *actor, const char *address)
{
    model_meta v;
    int rc, e;
    if (flock(m->lock_fd, LOCK_SH)) return -1;
    rc = readable(m, actor, address, &v); e = errno;
    flock(m->lock_fd, LOCK_UN); errno = e; return rc;
}
int model_send(model *m, const char *actor, const char *address,
               const char *content, size_t len, off_t *end)
{
    model_meta v;
    char p[1200], text[MODEL_CONTENT_MAX * 6 + 3], json[MODEL_JSON_MAX], tail;
    int fd = -1, rc = -1, n, e;
    off_t start;
    if (!content || !len || len > MODEL_CONTENT_MAX || !utf8(content, len)) return fail(EINVAL);
    if (flock(m->lock_fd, LOCK_SH)) return -1;
    if (actor_user(m, actor) || get(m, address, &v)) goto out;
    if (path(m, p, sizeof p, v.id, "messages.jsonl")) goto out;
    fd = open(p, O_RDWR | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    if (fd < 0 || flock(fd, LOCK_EX)) goto out;
    start = lseek(fd, 0, SEEK_END);
    if (start < 0) goto out;
    /* Do not append to crash debris: operator must repair the damaged tail. */
    if (start && (pread(fd, &tail, 1, start - 1) != 1 || tail != '\n')) { errno = EIO; goto out; }
    quote(text, content, len);
    n = snprintf(json, sizeof json, "{\"ts\":%lld,\"from\":\"%s\",\"to\":\"%s\",\"content\":%s}\n",
                 (long long)time(NULL), actor, v.id, text);
    if (n <= 0 || (size_t)n >= sizeof json) { errno = EOVERFLOW; goto out; }
    if (allwrite(fd, json, (size_t)n) || fsync(fd)) {
        e = errno;
        if (ftruncate(fd, start)) e = EIO;
        errno = e; goto out;
    }
    *end = start + n; rc = 0;
out:
    e = errno;
    if (fd >= 0) close(fd);
    flock(m->lock_fd, LOCK_UN); errno = e; return rc;
}
int model_read(model *m, const char *actor, const char *address,
               off_t *offset, model_emit emit, void *ctx)
{
    model_meta v;
    char p[1200], line[MODEL_JSON_MAX], tail;
    FILE *f = NULL;
    int fd = -1, rc = -1, e;
    struct stat st;
    if (!offset || *offset < 0 || !emit) return fail(EINVAL);
    if (flock(m->lock_fd, LOCK_SH)) return -1;
    if (readable(m, actor, address, &v) || path(m, p, sizeof p, v.id, "messages.jsonl")) goto out;
    fd = open(p, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { if (errno == ENOENT && !*offset) rc = 0; goto out; }
    if (flock(fd, LOCK_SH) || fstat(fd, &st)) goto out;
    if (*offset > st.st_size || (*offset &&
        (pread(fd, &tail, 1, *offset - 1) != 1 || tail != '\n'))) { errno = EINVAL; goto out; }
    f = fdopen(fd, "r");
    if (!f) goto out;
    fd = -1;
    if (fseeko(f, *offset, SEEK_SET)) goto out;
    rc = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        int accepted;
        if (!n || line[n-1] != '\n') { rc = fail(EIO); break; }
        accepted = emit(ctx, *offset + (off_t)n, line, n);
        if (accepted) { if (accepted < 0) rc = -1; break; }
        *offset += (off_t)n;
    }
    if (ferror(f)) rc = -1;
out:
    e = errno;
    if (f) fclose(f);
    if (fd >= 0) close(fd);
    flock(m->lock_fd, LOCK_UN); errno = e; return rc;
}
int model_list(model *m, const char *actor, model_visit visit, void *ctx)
{
    char p[1200];
    DIR *dp = NULL;
    struct dirent *de;
    model_meta v;
    int rc = -1, e;
    if (!visit) return fail(EINVAL);
    if (flock(m->lock_fd, LOCK_SH)) return -1;
    if (actor_user(m, actor)) goto out;
    snprintf(p, sizeof p, "%s/rooms", m->root);
    dp = opendir(p);
    if (!dp) goto out;
    rc = 0;
    while ((de = readdir(dp))) {
        if (!model_id_valid(de->d_name)) continue;
        if (load_meta(m, de->d_name, &v)) { rc = -1; break; }
        if (!strcmp(v.type, "room") || !strcmp(actor, v.id)) {
            rc = visit(ctx, &v);
            if (rc) break;
        }
    }
out:
    e = errno; if (dp) closedir(dp);
    flock(m->lock_fd, LOCK_UN); errno = e; return rc;
}
