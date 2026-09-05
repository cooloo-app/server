/* main.c - cooloo v0 entry point and dispatch
 *
 * Commands:
 *   cooloo send <room> [text...]    append a line, stdout: OK <end_offset>
 *   cooloo read <room> <offset>     read from byte offset
 *   cooloo read <room> -<N>         read last N lines
 *   cooloo follow <room> [offset]   stream new messages (offset omitted = tail)
 *   cooloo rooms                    list rooms
 *   cooloo whoami                   print nick (gate mode only)
 *   cooloo __gate <nick>            SSH forced-command entry (D8)
 */
#include "cooloo.h"
#include "util.h"
#include "cmd_send.h"
#include "cmd_read.h"
#include "cmd_follow.h"
#include "cmd_misc.h"
#include "cmd_gate.h"

#include <string.h>
#include <stdio.h>

static void usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  cooloo send <room> [text...]    append a line, stdout: OK <end_offset>\n"
            "  cooloo read <room> <offset>     read from byte offset\n"
            "  cooloo read <room> -<N>         read last N lines\n"
            "  cooloo follow <room> [offset]   stream new messages\n"
            "  cooloo rooms                    list rooms\n"
            "  cooloo whoami                   print nick (gate mode only)\n");
}

int cooloo_dispatch(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return COOLOO_EXIT_ARG;
    }
    if (strcmp(argv[1], "send") == 0)
        return cooloo_cmd_send(argc, argv);
    if (strcmp(argv[1], "read") == 0)
        return cooloo_cmd_read(argc, argv);
    if (strcmp(argv[1], "follow") == 0)
        return cooloo_cmd_follow(argc, argv);
    if (strcmp(argv[1], "rooms") == 0)
        return cooloo_cmd_rooms(argc, argv);
    if (strcmp(argv[1], "whoami") == 0)
        return cooloo_cmd_whoami(argc, argv);

    fprintf(stderr, "ERR unknown command: %s\n", argv[1]);
    return COOLOO_EXIT_ARG;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return COOLOO_EXIT_ARG;
    }
    if (strcmp(argv[1], "__gate") == 0)
        return cooloo_cmd_gate(argc, argv);
    return cooloo_dispatch(argc, argv);
}
