/* regex.h : public interface for the compact regex engine
 * SPDX-License-Identifier: 0BSD OR CC0-1.0
 */

#ifndef REGEX_H
#define REGEX_H

#include <stddef.h>     /* size_t */

/* Compile and match flags. The match flags (bits 0..2) are stored in the
 * compiled object; RX_GLOBAL only affects rx_replace. */
#define RX_ICASE     0x01   /* case-insensitive matching           */
#define RX_MULTILINE 0x02   /* ^ and $ match at embedded newlines   */
#define RX_DOTALL    0x04   /* . also matches newline               */
#define RX_GLOBAL    0x08   /* rx_replace: replace every match      */

typedef struct rx rx_t;

/* Offsets of a capture into the subject text, or -1/-1 when unset.
 * Index 0 is always the whole match. */
typedef struct {
    long so;
    long eo;
} rx_match;

/* Compile `pattern`. On failure returns NULL and, if errp is non-NULL,
 * stores a static error string in *errp. */
rx_t *rx_compile(const char *pattern, int flags, const char **errp);

/* Free a compiled pattern. */
void rx_free(rx_t *re);

/* Number of capturing groups (group 0 excluded). */
int rx_ngroups(const rx_t *re);

/* Search `text` (of `len` bytes) from offset `start`. Fills up to
 * `nmatch` entries of `m`. Returns 1 on a match, 0 when none is found,
 * or -1 on error (such as the step budget being exceeded). */
int rx_exec(rx_t *re, const char *text, size_t len, size_t start,
            rx_match *m, int nmatch);

/* Substitute matches of `re` in `text` using the `repl` template and
 * return a freshly malloc'd NUL-terminated string the caller frees.
 * With RX_GLOBAL every match is replaced, otherwise only the first.
 * Returns NULL on allocation failure or a matching error. */
char *rx_replace(rx_t *re, const char *text, size_t len, const char *repl,
                 int flags);

#endif /* REGEX_H */
