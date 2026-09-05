/* util.h - error reporting, path helpers, validation */
#ifndef COOLOO_UTIL_H
#define COOLOO_UTIL_H

#include <stddef.h>

/* Print "ERR <msg>\n" to stderr and exit with code. */
void cooloo_die(int code, const char *msg);
void cooloo_dief(int code, const char *fmt, const char *arg);

/* Data home: $COOLOO_HOME or COOLOO_DEFAULT_HOME. */
const char *cooloo_home(void);

/* Build "<home>/rooms" and "<home>/rooms/<room>.log" into buf.
 * Returns 0 on success, -1 if buf too small. */
int cooloo_rooms_dir(char *buf, size_t buflen);
int cooloo_room_path(char *buf, size_t buflen, const char *room);

/* Validation (D11): room must match ^[a-z0-9][a-z0-9_-]{0,31}$ */
int cooloo_room_valid(const char *room);

#endif /* COOLOO_UTIL_H */
