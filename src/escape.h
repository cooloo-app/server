/* escape.h - one-line-per-message invariant (D10)
 *
 * Storage escaping, applied on write:
 *   '\'  -> "\\"
 *   LF   -> "\n"  (two chars)
 *   CR   -> "\r"  (two chars)
 *   other control chars -> stripped
 * Read path keeps the escaped form; display side unescapes.
 */
#ifndef COOLOO_ESCAPE_H
#define COOLOO_ESCAPE_H

#include <stddef.h>

/* Escape src into dst (dst capacity dstlen, NUL-terminated on success).
 * Returns escaped length, or -1 if it would exceed dstlen or
 * COOLOO_MAX_MSG. */
long cooloo_escape(char *dst, size_t dstlen, const char *src, size_t srclen);

#endif /* COOLOO_ESCAPE_H */
