/* cmd_gate.c - SSH forced-command entry
 *
 * authorized_keys line:
 *   command="/usr/local/bin/cooloo __gate <nick>",restrict ssh-ed25519 ...
 *
 * Everything after `__gate <nick>` arrives via SSH_ORIGINAL_COMMAND as one
 * flat string; quoting was already destroyed by the SSH layer, so we split
 * on whitespace only. Multi-word messages should use stdin mode:
 *   echo "hello world" | ssh cooloo@host send general
 */
#include "cmd_gate.h"
#include "cooloo.h"
#include "util.h"
#include "cmd_send.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COOLOO_GATE_MAX_ARGS 32

static const char *g_gate_nick = NULL;

const char *cooloo_gate_nick(void)
{
    return g_gate_nick;
}

/* nick from authorized_keys is trusted, but validate anyway (defense):
 * [a-z0-9_-]{1,16} */
static int nick_valid(const char *nick)
{
    size_t len, i;

    if (!nick)
        return 0;
    len = strlen(nick);
    if (len < 1 || len > COOLOO_MAX_NICK)
        return 0;
    for (i = 0; i < len; i++) {
        char c = nick[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

static void welcome(const char *nick)
{
    printf("Welcome to cooloo, %s!\n", nick);
    printf("usage:\n");
    printf("  send <room> [text...]     append a message (stdin if no text)\n");
    printf("  read <room> <offset>      read from byte offset\n");
    printf("  read <room> -<N>          read last N lines\n");
    printf("  follow <room> [offset]    stream new messages\n");
    printf("  whoami                    print your nick\n");
    printf("  rooms                     list rooms\n");
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int cooloo_cmd_gate(int argc, char **argv)
{
    static char cmd[COOLOO_MAX_MSG + 256];
    static char *dargv[COOLOO_GATE_MAX_ARGS + 2];
    const char *nick, *oc;
    int dargc = 1;              /* dargv[0] reserved for program name */
    char *p;

    if (argc < 3)
        cooloo_die(COOLOO_EXIT_ARG, "usage: cooloo __gate <nick>");
    nick = argv[2];
    if (!nick_valid(nick))
        cooloo_die(COOLOO_EXIT_ARG, "invalid nick");

    g_gate_nick = nick;
    cooloo_send_set_nick(nick);

    oc = getenv("SSH_ORIGINAL_COMMAND");
    if (!oc || !oc[0]) {
        welcome(nick);
        return 0;
    }

    /* Split on whitespace into dargv[1..] */
    snprintf(cmd, sizeof(cmd), "%s", oc);
    p = cmd;
    while (*p) {
        while (*p && is_space(*p))
            p++;
        if (!*p)
            break;
        if (dargc > COOLOO_GATE_MAX_ARGS)
            cooloo_die(COOLOO_EXIT_ARG, "too many arguments");
        dargv[dargc++] = p;
        while (*p && !is_space(*p))
            p++;
        if (*p)
            *p++ = '\0';
    }
    if (dargc == 1) {
        welcome(nick);
        return 0;
    }

    dargv[0] = (char *)"cooloo";
    dargv[dargc] = NULL;
    return cooloo_dispatch(dargc, dargv);
}
