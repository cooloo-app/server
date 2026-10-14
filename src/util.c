/* util.c - see util.h */
#include "util.h"
#include "cooloo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>

void cooloo_die(int code, const char *msg)
{
    fprintf(stderr, "ERR %s\n", msg);
    exit(code);
}

void cooloo_dief(int code, const char *fmt, const char *arg)
{
    fprintf(stderr, "ERR ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n");
    exit(code);
}

const char *cooloo_home(void)
{
    const char *h = getenv("COOLOO_HOME");
    static char path[1024];
    const char *home;
    int n;
    if (h && h[0]) return h;
    home = getenv("HOME");
    if (!home || !home[0]) cooloo_die(1, "HOME required or set COOLOO_HOME");
    n = snprintf(path, sizeof path, "%s/.cooloo", home);
    if (n < 0 || (size_t)n >= sizeof path) cooloo_die(1, "HOME too long");
    if (mkdir(path, 0700) && errno != EEXIST) cooloo_die(1, "cannot create ~/.cooloo");
    n = snprintf(path, sizeof path, "%s/.cooloo/server", home);
    if (n < 0 || (size_t)n >= sizeof path) cooloo_die(1, "HOME too long");
    return path;
}

int cooloo_rooms_dir(char *buf, size_t buflen)
{
    int n = snprintf(buf, buflen, "%s/rooms", cooloo_home());
    return (n > 0 && (size_t)n < buflen) ? 0 : -1;
}

int cooloo_room_path(char *buf, size_t buflen, const char *room)
{
    int n = snprintf(buf, buflen, "%s/rooms/%s.log", cooloo_home(), room);
    return (n > 0 && (size_t)n < buflen) ? 0 : -1;
}

int cooloo_room_valid(const char *room)
{
    size_t len, i;

    if (!room)
        return 0;
    len = strlen(room);
    if (len < 1 || len > COOLOO_MAX_ROOM)
        return 0;
    if (room[0] < 'a' || room[0] > 'z') {
        if (room[0] < '0' || room[0] > '9')
            return 0;
    }
    for (i = 1; i < len; i++) {
        char c = room[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}
