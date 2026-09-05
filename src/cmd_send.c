/* cmd_send.c - append a line to a room
 *
 * Two input modes (D9):
 *   cooloo send <room> <text...>   text = rest of argv joined by spaces,
 *                                  trimmed at both ends
 *   cooloo send <room>             text = stdin read to EOF (multi-line ok,
 *                                  stored escaped as one line)
 * Nick: $USER when run directly (admin debugging on the server host).
 */
#include "cmd_send.h"
#include "cooloo.h"
#include "util.h"
#include "escape.h"
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_nick_override = NULL;

void cooloo_send_set_nick(const char *nick)
{
    g_nick_override = nick;
}

/* Join argv[3..] with single spaces, trim leading/trailing whitespace. */
static size_t join_args(char *buf, size_t buflen, int argc, char **argv)
{
    size_t len = 0;
    int i;

    buf[0] = '\0';
    for (i = 3; i < argc; i++) {
        size_t al = strlen(argv[i]);
        if (len && len + 1 < buflen)
            buf[len++] = ' ';
        if (len + al >= buflen)
            al = buflen - len - 1;
        memcpy(buf + len, argv[i], al);
        len += al;
    }
    buf[len] = '\0';

    /* trim leading */
    {
        size_t start = 0;
        while (buf[start] == ' ' || buf[start] == '\t')
            start++;
        if (start) {
            memmove(buf, buf + start, len - start + 1);
            len -= start;
        }
    }
    /* trim trailing */
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = '\0';
    return len;
}

/* Read stdin to EOF into buf. Returns length read (may be truncated
 * at buflen-1; escape step enforces the real limit). */
static size_t read_stdin(char *buf, size_t buflen)
{
    size_t len = 0;
    int c;

    while ((c = getchar()) != EOF) {
        if (len + 1 >= buflen)
            break;
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    /* trim trailing newlines/spaces for stdin mode */
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                   buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = '\0';
    return len;
}

int cooloo_cmd_send(int argc, char **argv)
{
    static char raw[COOLOO_MAX_MSG * 2];
    static char esc[COOLOO_MAX_MSG + 1];
    const char *room, *nick;
    size_t rawlen;
    long esclen;
    off_t end;

    if (argc < 3)
        cooloo_die(COOLOO_EXIT_ARG, "usage: cooloo send <room> [text...]");
    room = argv[2];
    if (!cooloo_room_valid(room))
        cooloo_dief(COOLOO_EXIT_ARG, "invalid room name: %s", room);

    if (argc > 3)
        rawlen = join_args(raw, sizeof(raw), argc, argv);
    else
        rawlen = read_stdin(raw, sizeof(raw));
    if (rawlen == 0)
        cooloo_die(COOLOO_EXIT_ARG, "empty message");

    esclen = cooloo_escape(esc, sizeof(esc), raw, rawlen);
    if (esclen < 0)
        cooloo_die(COOLOO_EXIT_ARG, "message too long (max 4096 escaped bytes)");

    nick = g_nick_override;
    if (!nick || !nick[0])
        nick = getenv("USER");
    if (!nick || !nick[0])
        nick = "unknown";

    end = cooloo_store_append(room, nick, esc);
    if (end < 0)
        cooloo_die(COOLOO_EXIT_IO, "io error writing room log");

    printf("OK %lld\n", (long long)end);
    return 0;
}
