#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static char a[65], b[65], c[65], d[65];
static int count;
static int emit(void *ctx, off_t end, const char *line, size_t n)
{
    (void)ctx; (void)end;
    assert(n && line[n-1] == '\n');
    assert(strstr(line, "\"from\":\""));
    count++;
    return 0;
}
static int stop(void *ctx, off_t end, const char *line, size_t n)
{ (void)ctx; (void)end; (void)line; (void)n; return 1; }
static int list(void *ctx, const model_meta *v)
{
    int *n = ctx;
    assert(!strcmp(v->type, "room") || !strcmp(v->id, a));
    (*n)++; return 0;
}
int main(int argc, char **argv)
{
    model m;
    model_meta v;
    char room[65], alias[70], p[1200];
    off_t end, off = 0;
    int n = 0, status, i;
    FILE *f;
    pid_t pid;
    assert(argc == 2);
    memset(a, 'a', 64); memset(b, 'b', 64); memset(c, 'c', 64); memset(d, 'd', 64);
    assert(!model_open(&m, argv[1]));
    assert(!model_register(&m, a, "同名 \"user\" \\"));
    assert(!model_register(&m, b, "同名 \"user\" \\"));
    assert(!model_get(&m, a, &v) && !strcmp(v.nickname, "同名 \"user\" \\"));
    assert(model_register(&m, a, "other") < 0 && errno == EEXIST);
    assert(model_register(&m, "../../oops", "x") < 0);
    assert(!model_avatar(&m, a, 1, c, "agent"));
    assert(model_avatar(&m, a, 1, d, "agent") < 0 && errno == EEXIST);
    assert(model_avatar(&m, a, 2, b, "claim") < 0 && errno == EEXIST);
    assert(model_avatar(&m, c, 1, d, "nested") < 0 && errno == EACCES);
    assert(model_avatar(&m, a, 900, d, "reserved") < 0);
    snprintf(alias, sizeof alias, "%s:001", a);
    assert(!model_get(&m, alias, &v) && !strcmp(v.id, c));
    assert(!model_send(&m, b, alias, "hello\n\"世界\"\t\\", strlen("hello\n\"世界\"\t\\"), &end));
    assert(model_can_read(&m, a, alias) < 0 && errno == EACCES);
    assert(model_can_read(&m, b, alias) < 0 && errno == EACCES);
    assert(!model_can_read(&m, c, alias));
    assert(!model_read(&m, c, alias, &off, stop, NULL) && off == 0);
    assert(!model_read(&m, c, alias, &off, emit, NULL) && off == end && count == 1);
    off = 1;
    assert(model_read(&m, c, alias, &off, emit, NULL) < 0 && errno == EINVAL);
    assert(model_rename(&m, c, a, "bad") < 0 && errno == EACCES);
    assert(!model_rename(&m, a, c, "renamed"));
    assert(!model_room(&m, a, "public", room));
    assert(!model_can_read(&m, b, room));
    assert(!model_list(&m, a, list, &n) && n == 2);
    assert(model_send(&m, d, room, "unregistered", 12, &end) < 0);
    assert(model_send(&m, b, room, "\xc0\x80", 2, &end) < 0);
    assert(model_send(&m, b, room, "a\0b", 3, &end) < 0);
    /* Independent processes append whole, durable JSONL records. */
    pid = fork(); assert(pid >= 0);
    if (!pid) {
        model child;
        assert(!model_open(&child, argv[1]));
        for (i = 0; i < 40; i++) assert(!model_send(&child, b, room, "child", 5, &end));
        model_close(&child); _exit(0);
    }
    for (i = 0; i < 40; i++) assert(!model_send(&m, a, room, "parent", 6, &end));
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    off = 0; count = 0;
    assert(!model_read(&m, b, room, &off, emit, NULL) && count == 80);
    model_close(&m);
    assert(!model_open(&m, argv[1]));
    assert(!model_get(&m, alias, &v) && !strcmp(v.nickname, "renamed"));
    snprintf(p, sizeof p, "%s/rooms/%s/messages.jsonl", argv[1], room);
    f = fopen(p, "a"); assert(f); fputs("{incomplete", f); assert(!fclose(f));
    assert(model_send(&m, a, room, "no", 2, &end) < 0 && errno == EIO);
    model_close(&m);
    puts("model: identity, alias, ACL, UTF-8, JSONL, backpressure, concurrent append and restart passed");
    return 0;
}
