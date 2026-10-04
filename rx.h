/* rx.h : public interface for the compact regex engine
 * SPDX-License-Identifier: 0BSD OR CC0-1.0
 */

#ifndef RX_H
#define RX_H

#include <stddef.h>     /* size_t */

/* Library version. RX_VERSION is a single integer for comparisons, for
 * example: #if RX_VERSION >= RX_VERSION_MAKE(1, 2, 0) */
#define RX_VERSION_MAJOR 1
#define RX_VERSION_MINOR 0
#define RX_VERSION_PATCH 0
#define RX_VERSION_STRING "1.0.0"
#define RX_VERSION_MAKE(maj, min, pat) ((maj) * 10000 + (min) * 100 + (pat))
#define RX_VERSION \
    RX_VERSION_MAKE(RX_VERSION_MAJOR, RX_VERSION_MINOR, RX_VERSION_PATCH)

/* Compile and match flags. The match flags (bits 0..2) are stored in the
 * compiled object; RX_GLOBAL only affects rx_replace. */
#define RX_ICASE     0x01   /* case-insensitive matching           */
#define RX_MULTILINE 0x02   /* ^ and $ match at embedded newlines   */
#define RX_DOTALL    0x04   /* . also matches newline               */
#define RX_GLOBAL    0x08   /* rx_replace: replace every match      */

/* Search flags for rx_search. These occupy a separate bit range from the
 * compile flags above and are passed only to rx_search. */
#define RX_BACKWARD  0x10   /* search toward the start of the text  */
#define RX_WRAP      0x20   /* wrap past the far end on a miss       */

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

/* Report whether a match of `re` can contain a newline byte. Returns 0
 * when every possible match lies within a single line, which lets an
 * editor safely search and highlight one line at a time; returns 1 when
 * a match may span lines, so the whole buffer must be searched at once. */
int rx_matches_newline(const rx_t *re);

/* Editor-oriented search, convenient for incremental (type-as-you-go)
 * search. By default it finds the first match beginning at or after
 * `from`; with RX_BACKWARD it finds the last match beginning before
 * `from`. With RX_WRAP a miss continues from the far end of the text so
 * the whole buffer is covered, as in an editor's search. `m`, `nmatch`,
 * and the 1/0/-1 return value are exactly as for rx_exec. Backward
 * search considers non-overlapping matches and costs a scan of the
 * searched span. */
int rx_search(rx_t *re, const char *text, size_t len, size_t from,
              int sflags, rx_match *m, int nmatch);

/* Substitute matches of `re` in `text` using the `repl` template and
 * return a freshly malloc'd NUL-terminated string the caller frees.
 * With RX_GLOBAL every match is replaced, otherwise only the first.
 * Returns NULL on allocation failure or a matching error. */
char *rx_replace(rx_t *re, const char *text, size_t len, const char *repl,
                 int flags);

#endif /* RX_H */
