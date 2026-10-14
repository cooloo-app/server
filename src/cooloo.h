/* cooloo.h - shared constants for cooloo v0
 *
 * v0 core idea: SSH is the transport. Server is a single small binary,
 * storage is append-only plain text files under $COOLOO_HOME.
 */
#ifndef COOLOO_H
#define COOLOO_H

/* Limits (see handoff D11/D13) */
#define COOLOO_MAX_MSG   4096   /* max escaped message bytes, single write() */
#define COOLOO_MAX_ROOM  68     /* public ID plus optional :NNN alias */
#define COOLOO_MAX_NICK  64     /* nickname UTF-8 bytes */
#define COOLOO_TAIL_MAX  1000   /* read -N upper bound */

/* Default data dir, overridable via env COOLOO_HOME (D14) */
#define COOLOO_DEFAULT_HOME "/var/cooloo"

/* Exit codes: io=1, usage/args=2 */
#define COOLOO_EXIT_IO   1
#define COOLOO_EXIT_ARG  2

/* Shared dispatch used by both main() and __gate.
 * argv[0] is the program name, argv[1] the subcommand. */
int cooloo_dispatch(int argc, char **argv);

#endif /* COOLOO_H */
