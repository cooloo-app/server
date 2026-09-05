/* cmd_read.c - read room log from a byte offset, or the last N lines */
#include "cmd_read.h"
#include "cooloo.h"
#include "util.h"
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Parse a non-negative byte offset. Returns 0 on success. */
static int parse_offset(const char *s, off_t *out)
{
    char *end;
    long long v;

    if (!s || !s[0])
        return -1;
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno || *end || v < 0)
        return -1;
    *out = (off_t)v;
    return 0;
}

/* Parse "-N" tail argument. Returns n >= 1, or -1 on bad input. */
static long parse_tail(const char *s)
{
    char *end;
    long v;

    if (!s || s[0] != '-' || !s[1])
        return -1;
    errno = 0;
    v = strtol(s + 1, &end, 10);
    if (errno || *end || v < 1)
        return -1;
    if (v > COOLOO_TAIL_MAX)
        v = COOLOO_TAIL_MAX;
    return v;
}

int cooloo_cmd_read(int argc, char **argv)
{
    const char *room;

    if (argc < 4)
        cooloo_die(COOLOO_EXIT_ARG,
                   "usage: cooloo read <room> <offset> | cooloo read <room> -<N>");
    room = argv[2];
    if (!cooloo_room_valid(room))
        cooloo_dief(COOLOO_EXIT_ARG, "invalid room name: %s", room);

    if (argv[3][0] == '-') {
        long n = parse_tail(argv[3]);
        if (n < 0)
            cooloo_dief(COOLOO_EXIT_ARG, "bad tail count: %s", argv[3]);
        if (cooloo_store_tail(room, n) != 0)
            cooloo_die(COOLOO_EXIT_IO, "io error reading room log");
    } else {
        off_t off = 0;
        if (parse_offset(argv[3], &off) != 0)
            cooloo_dief(COOLOO_EXIT_ARG, "bad offset: %s", argv[3]);
        if (cooloo_store_read(room, off) != 0)
            cooloo_die(COOLOO_EXIT_IO, "io error reading room log");
    }
    return 0;
}
