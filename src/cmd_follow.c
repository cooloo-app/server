/* cmd_follow.c - follow a room log (D15: poll file size every 100ms)
 *
 * Signal note: over SSH with `restrict` (no pty), client Ctrl-C does NOT
 * deliver SIGINT to us; connection teardown shows up as SIGHUP. Handle all
 * three so we always exit cleanly.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "cmd_follow.h"
#include "cooloo.h"
#include "util.h"
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static off_t room_file_size(const char *room)
{
    char path[1024];
    struct stat st;

    if (cooloo_room_path(path, sizeof(path), room) != 0)
        return 0;
    if (stat(path, &st) != 0)
        return 0;
    return st.st_size;
}

int cooloo_cmd_follow(int argc, char **argv)
{
    const char *room;
    off_t off;
    struct timespec ts;

    if (argc < 3)
        cooloo_die(COOLOO_EXIT_ARG, "usage: cooloo follow <room> [offset]");
    room = argv[2];
    if (!cooloo_room_valid(room))
        cooloo_dief(COOLOO_EXIT_ARG, "invalid room name: %s", room);

    if (argc >= 4) {
        char *end;
        long long v;
        errno = 0;
        v = strtoll(argv[3], &end, 10);
        if (errno || *end || v < 0)
            cooloo_dief(COOLOO_EXIT_ARG, "bad offset: %s", argv[3]);
        off = (off_t)v;
    } else {
        off = room_file_size(room);     /* default: tail, only new messages */
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    ts.tv_sec = 0;
    ts.tv_nsec = 100 * 1000 * 1000;     /* 100ms */
    while (!g_stop) {
        if (cooloo_store_read_update(room, &off) != 0)
            cooloo_die(COOLOO_EXIT_IO, "io error reading room log");
        fflush(stdout); /* SSH pipe: must not block-buffer */
        nanosleep(&ts, NULL);
    }
    return 0;
}
