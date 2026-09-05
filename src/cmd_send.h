/* cmd_send.h - `cooloo send <room> [text...]` */
#ifndef COOLOO_CMD_SEND_H
#define COOLOO_CMD_SEND_H

/* argc/argv are the full process args; argv[1] == "send".
 * Returns exit code (0 on success). Prints "OK <end_offset>" on stdout. */
int cooloo_cmd_send(int argc, char **argv);

/* Override the nick used by send (called by __gate; default is $USER). */
void cooloo_send_set_nick(const char *nick);

#endif /* COOLOO_CMD_SEND_H */
