# regex

A compact regular expression engine in C, meant to be dropped into a
small text tool such as a sed, a vi clone, or a grep. It is a
backtracking matcher, so it supports backreferences and the common
conveniences found in GNU sed, Vim, and PCRE on top of the POSIX ERE
core.

The engine is two files: `rx.c` and `rx.h`. Drop both into a
project and you have a working regex library. `rx.c` also carries a
sed-like command-line tool behind the `RX_MAIN` compile flag. The tests
live under `tests/`.

## Building

```sh
make test   # build and run the self-test suite
make cli    # build the rsed command-line tool
make clean
```

`make cli` writes the `rsed` binary to the top level. The test targets
build their binaries under `tests/` (`tests/selftest`, `tests/torturet`,
and so on) and `make clean` removes all of them. `CC` and `CFLAGS` are
overridable, for example `make test CC=clang`.

## Testing

```sh
make test      # fast public-API self-test (tests/selftest.c)
make torture   # heavy hand-verified battery, fault sweep, and fuzzer
make asan      # torture suite under AddressSanitizer + leak detection
make ubsan     # torture suite under UndefinedBehaviorSanitizer
make fault     # allocation-failure sweep under ASan (fuzzer skipped)
make valgrind  # the same sweep under valgrind memcheck
make cov       # line coverage; writes tests/rx.c.gcov
```

The test sources live under `tests/`:

- `tests/selftest.c` is a fast smoke test that drives only the public
  API and doubles as a worked example of using it.
- `tests/torture.c` is the heavy suite: a hand-verified correctness
  battery, the allocation-failure sweep, and the fuzzer. It includes
  `rx.c` directly so the sanitizers and `gcov` see the whole engine as
  one translation unit, and so the fault sweep can hook its allocator.

Both files are organized the same way. Each group of checks is a small
single-concern function (for example `t_bracket_classes` or
`t_search_directions`), a section driver such as `battery()` is just a
list of those calls, and `main()` runs the drivers in order. To add a
case, extend the matching function; to add a group, write a function and
add one call to its driver. A handful of assertion helpers
(`ck_match`, `ck_group`, `ck_sub`, `ck_badpat`, `ck_nl`, `ck_search`)
each run one check and bump the pass/fail counters.

The torture target takes an optional iteration count for the fuzzer, for
example `./tests/torturet 100000`; `0` skips the fuzzer.

The fault sweep routes every allocation in the engine through a hook that
fails the Nth allocation in turn. For each operation it first counts the
allocations, then re-runs it once per allocation, failing exactly one and
asserting the documented failure value is returned. Run under
AddressSanitizer, the leak detector confirms every error path releases
its partial state; `make valgrind` re-runs the sweep under valgrind
memcheck as an independent check.

The suites pass clean under ASan and UBSan with no leaks or undefined
behavior. Coverage of `rx.c` is about 97%; the remainder is a handful
of defensively-unreachable branches and allocation-failure paths that are
only reachable at a specific reallocation boundary.

To embed the engine in another program, copy `rx.c` and `rx.h`
into your source tree, include `rx.h` where you call the API, and
compile `rx.c` as an ordinary translation unit (with `RX_MAIN`
undefined). For example:

```sh
cc -c rx.c -o rx.o
cc yourprog.c rx.o -o yourprog
```

`rx.h` is the whole public interface. The files are named `rx.*` rather
than `regex.*` so the header does not clash with the POSIX `<regex.h>`.
A few compile-time limits can be overridden with `-D` when building
`rx.c`: `RX_STEP_LIMIT`, `RX_DUP_MAX`, and `RX_MAX_DEPTH`.

## Pattern syntax

The base is POSIX ERE. A backslash makes a metacharacter literal.

| Syntax | Meaning |
| --- | --- |
| `.` | any character (not newline unless `RX_DOTALL`) |
| `*` `+` `?` | greedy quantifiers |
| `*?` `+?` `??` | lazy (non-greedy) quantifiers |
| `{n}` `{n,}` `{n,m}` `{,m}` | counted repetition (lazy variants with trailing `?`) |
| `[...]` `[^...]` | bracket expression, with ranges, `[:class:]`, and `\d \w \s` shorthands |
| `^` `$` | anchors (line anchors under `RX_MULTILINE`) |
| `(...)` `(?:...)` | capturing and non-capturing groups |
| `\|` | alternation |
| `\1` .. `\9` | backreference |
| `\d \D \w \W \s \S` | shorthand classes |
| `\b \B` | word boundary, non-boundary |
| `\< \>` | start-of-word, end-of-word |
| `\n \t \r \f \v \a \0` | control escapes, plus `\xHH` |

POSIX bracket classes: `[:alpha:]`, `[:digit:]`, `[:alnum:]`,
`[:space:]`, `[:upper:]`, `[:lower:]`, `[:punct:]`, `[:xdigit:]`,
`[:cntrl:]`, `[:print:]`, `[:graph:]`, `[:blank:]`.

Matching is leftmost with greedy-by-default preference (Perl and PCRE
semantics), not POSIX leftmost-longest.

## Replacement template

Used by `rx_replace` and the `\2.\1`-style second argument to `rsed`.

| Syntax | Meaning |
| --- | --- |
| `&` or `\0` | the whole match |
| `\1` .. `\9` | a captured group |
| `\U` `\L` | upcase / downcase following output until `\E` |
| `\u` `\l` | upcase / downcase the next output character |
| `\E` | end case conversion |
| `\n \t \r \f \v \a` | control escapes, plus `\xHH` |
| `\&` `\\` | literal `&` and backslash |

## Flags

Passed to `rx_compile` (match flags) and `rx_replace` (`RX_GLOBAL`):

- `RX_ICASE` — case-insensitive matching
- `RX_MULTILINE` — `^` and `$` also match at embedded newlines
- `RX_DOTALL` — `.` also matches newline
- `RX_GLOBAL` — `rx_replace` replaces every match, not just the first

## Command-line tool

```
rsed [-gims] PATTERN REPLACEMENT < input
```

- `-g` global, `-i` ignore case, `-m` multiline, `-s` dotall.
- Reads stdin line by line and writes the substituted result to stdout.

Examples:

```sh
# swap user and host around an @ sign
echo 'user@host' | ./rsed -g '(\w+)@(\w+)' '\2.\1'
# -> host.user

# wrap every run of letters, upcased
echo 'hello world' | ./rsed -g '[[:alpha:]]+' '[\U&]'
# -> [HELLO] [WORLD]

# capitalize the first letter of a word
echo 'john' | ./rsed '(\w)(\w*)' '\u\1\2'
# -> John

# collapse runs of whitespace to a single space
printf 'a   b\t c\n' | ./rsed -g '\s+' ' '
# -> a b c
```

## C API

```c
#define RX_VERSION_STRING "1.0.0"   /* also RX_VERSION_MAJOR/MINOR/PATCH */
#define RX_VERSION ...              /* integer; compare via RX_VERSION_MAKE */

#define RX_ICASE     0x01
#define RX_MULTILINE 0x02
#define RX_DOTALL    0x04
#define RX_GLOBAL    0x08

#define RX_BACKWARD  0x10   /* rx_search: search toward the start */
#define RX_WRAP      0x20   /* rx_search: wrap past the far end    */

typedef struct rx rx_t;

typedef struct {
    long so;    /* start offset, or -1 if unset */
    long eo;    /* end offset, or -1 if unset   */
} rx_match;

rx_t *rx_compile(const char *pattern, int flags, const char **errp);
void  rx_free(rx_t *re);
int   rx_ngroups(const rx_t *re);
int   rx_exec(rx_t *re, const char *text, size_t len, size_t start,
              rx_match *m, int nmatch);
int   rx_matches_newline(const rx_t *re);
int   rx_search(rx_t *re, const char *text, size_t len, size_t from,
                int sflags, rx_match *m, int nmatch);
char *rx_replace(rx_t *re, const char *text, size_t len,
                 const char *repl, int flags);
```

`rx_exec` returns 1 on a match, 0 when none is found, or -1 on error.
Entry 0 of `m` is the whole match; entries 1..`rx_ngroups` are the
captured groups. `rx_replace` returns a freshly allocated,
NUL-terminated string that the caller frees.

## Editor integration

Two entry points help a text editor use the engine for search and
syntax highlighting.

`rx_matches_newline` reports whether a match of the compiled pattern can
contain a newline. When it returns 0, every possible match lies within a
single line, so the caller can take a fast path and run the matcher one
line at a time (for example, highlighting only the visible lines)
without missing a match. When it returns 1, because the pattern uses
`RX_DOTALL`, a literal newline, or a class that admits one, the caller
must search the whole buffer.

`rx_search` is a convenience over `rx_exec` for incremental,
type-as-you-go search. By default it finds the first match beginning at
or after `from`. With `RX_BACKWARD` it finds the last match beginning
before `from`. With `RX_WRAP` a miss continues from the far end so the
whole buffer is covered. The `m`, `nmatch`, and 1/0/-1 return are the
same as `rx_exec`. A typical loop recompiles the pattern on each
keystroke and calls `rx_search` from the point where the search began:

```c
rx_match m[1];
int hit = rx_search(re, buf, buflen, cursor,
                    backward ? RX_BACKWARD | RX_WRAP : RX_WRAP, m, 1);
if (hit == 1)
    highlight(m[0].so, m[0].eo);
```

Example:

```c
const char *err;
rx_t *re = rx_compile("(\\w+)@(\\w+)", 0, &err);
if (!re) {
    fprintf(stderr, "bad pattern: %s\n", err);
    return 1;
}

const char *s = "user@host";
rx_match m[3];
if (rx_exec(re, s, strlen(s), 0, m, 3) == 1) {
    printf("user: %.*s\n", (int)(m[1].eo - m[1].so), s + m[1].so);
    printf("host: %.*s\n", (int)(m[2].eo - m[2].so), s + m[2].so);
}

char *out = rx_replace(re, s, strlen(s), "\\2.\\1", 0);
printf("%s\n", out);    /* host.user */
free(out);

rx_free(re);
```

## Limitations

- A per-search instruction budget (`RX_STEP_LIMIT`) guards against
  catastrophic backtracking. A pattern that nests quantifiers over a
  nullable body, such as `(.*)*`, can still explore exponentially many
  states; the budget bounds that to an error return rather than a hang,
  but the pattern is better rewritten.
- Repetition counts in `{n,m}` are capped at `RX_DUP_MAX` (32767,
  matching the POSIX minimum). A larger count is a compile error.
- Group nesting is capped at `RX_MAX_DEPTH` (1000). The parser recurses
  on the C stack per nested group, so a deeper pattern is a compile error
  rather than a stack overflow.
- Matching is byte-oriented, not UTF-8 aware. Multibyte characters are
  matched as individual bytes.
