/* cmd_read.h - `cooloo read <room> <offset>` and `cooloo read <room> -<N>` */
#ifndef COOLOO_CMD_READ_H
#define COOLOO_CMD_READ_H

/* Returns exit code (0 on success). Output lines:
 *   <end_offset> <ts> <nick> <escaped_text> */
int cooloo_cmd_read(int argc, char **argv);

#endif /* COOLOO_CMD_READ_H */
