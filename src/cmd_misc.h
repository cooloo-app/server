/* cmd_misc.h - `cooloo whoami` and `cooloo rooms` */
#ifndef COOLOO_CMD_MISC_H
#define COOLOO_CMD_MISC_H

/* whoami: only meaningful in __gate mode (prints the gate nick). */
int cooloo_cmd_whoami(int argc, char **argv);

/* rooms: list room names under $COOLOO_HOME/rooms, one per line, sorted. */
int cooloo_cmd_rooms(int argc, char **argv);

#endif /* COOLOO_CMD_MISC_H */
