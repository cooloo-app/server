/* escape.c - see escape.h */
#include "escape.h"
#include "cooloo.h"

long cooloo_escape(char *dst, size_t dstlen, const char *src, size_t srclen)
{
    size_t di = 0, si;

    for (si = 0; si < srclen; si++) {
        unsigned char c = (unsigned char)src[si];
        const char *rep = NULL;
        size_t need = 1;

        if (c == '\\')
            rep = "\\\\", need = 2;
        else if (c == '\n')
            rep = "\\n", need = 2;
        else if (c == '\r')
            rep = "\\r", need = 2;
        else if (c < 0x20 || c == 0x7f)
            continue;           /* strip other control chars */

        if (di + need > COOLOO_MAX_MSG || di + need >= dstlen)
            return -1;
        if (rep) {
            dst[di++] = rep[0];
            dst[di++] = rep[1];
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
    return (long)di;
}
