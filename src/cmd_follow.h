/* cmd_follow.h - `cooloo follow <room> [offset]` */
#ifndef COOLOO_CMD_FOLLOW_H
#define COOLOO_CMD_FOLLOW_H

/* Stream new messages in read output format, never exits on its own.
 * offset omitted = start at file tail (only new messages). */
int cooloo_cmd_follow(int argc, char **argv);

#endif /* COOLOO_CMD_FOLLOW_H */
