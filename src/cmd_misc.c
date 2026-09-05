/* cmd_misc.c - whoami + rooms */
#define _DEFAULT_SOURCE

#include "cmd_misc.h"
#include "cooloo.h"
#include "util.h"
#include "cmd_gate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

int cooloo_cmd_whoami(int argc, char **argv)
{
    const char *nick;
    (void)argc;
    (void)argv;

    nick = cooloo_gate_nick();
    if (!nick)
        cooloo_die(COOLOO_EXIT_ARG, "whoami is only available via ssh gate");
    printf("%s\n", nick);
    return 0;
}

#define COOLOO_ROOMS_MAX 4096

static int cmp_room(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int cooloo_cmd_rooms(int argc, char **argv)
{
    static char names[COOLOO_ROOMS_MAX][COOLOO_MAX_ROOM + 1];
    char dir[1024];
    DIR *dp;
    struct dirent *de;
    int count = 0, i;
    (void)argc;
    (void)argv;

    if (cooloo_rooms_dir(dir, sizeof(dir)) != 0)
        cooloo_die(COOLOO_EXIT_IO, "rooms dir path too long");
    dp = opendir(dir);
    if (!dp)
        return 0;               /* no rooms dir yet: empty list */

    while ((de = readdir(dp)) != NULL && count < COOLOO_ROOMS_MAX) {
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

    qsort(names, (size_t)count, sizeof(names[0]), cmp_room);
    for (i = 0; i < count; i++)
        printf("%s\n", names[i]);
    return 0;
}
