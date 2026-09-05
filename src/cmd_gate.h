/* cmd_gate.h - `cooloo __gate <nick>` SSH forced-command entry (D8) */
#ifndef COOLOO_CMD_GATE_H
#define COOLOO_CMD_GATE_H

/* Gate entry: reads SSH_ORIGINAL_COMMAND, splits on whitespace (no
 * quoting, by design), dispatches to the normal subcommands.
 * Empty command prints welcome + usage and exits (no interactive shell). */
int cooloo_cmd_gate(int argc, char **argv);

/* The nick passed to __gate, or NULL when not in gate mode. */
const char *cooloo_gate_nick(void);

#endif /* COOLOO_CMD_GATE_H */
