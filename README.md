# regex

A compact, single-file regular expression engine in C, meant to be
dropped into a small text tool such as a sed, a vi clone, or a grep. It
is a backtracking matcher, so it supports backreferences and the common
conveniences found in GNU sed, Vim, and PCRE on top of the POSIX ERE
core.

Everything lives in `regex.c`: the public API, the engine, a self-test
harness, and a sed-like command-line tool.

## Building

```sh
make test   # build and run the self-test suite
make cli    # build the rsed command-line tool
make clean
```

`make cli` writes the `rsed` binary to the top level. The test targets
build their binaries under `tests/` (`tests/rxtest`, `tests/torturet`,
and so on) and `make clean` removes all of them. `CC` and `CFLAGS` are
overridable, for example `make test CC=clang`.

## Testing

```sh
make test      # the in-file self-test suite (regex.c -DRX_TEST)
make torture   # heavy hand-verified battery, fault sweep, and fuzzer
make asan      # torture suite under AddressSanitizer + leak detection
make ubsan     # torture suite under UndefinedBehaviorSanitizer
make fault     # allocation-failure sweep under ASan (fuzzer skipped)
make valgrind  # the same sweep under valgrind memcheck
make cov       # line coverage; writes tests/regex.c.gcov
```

The test sources live under `tests/`. `tests/torture.c` includes
`regex.c` directly so the sanitizers and `gcov` see the whole engine as
one translation unit. The torture target takes an optional iteration
count, for example `./tests/torturet 100000`.

The fault sweep routes every allocation in the engine through a hook that
fails the Nth allocation in turn. For each operation it first counts the
allocations, then re-runs it once per allocation, failing exactly one and
asserting the documented failure value is returned. Run under
AddressSanitizer, the leak detector confirms every error path releases
its partial state; `make valgrind` re-runs the sweep under valgrind
memcheck as an independent check.

The suites pass clean under ASan and UBSan with no leaks or undefined
behavior. Coverage of `regex.c` is about 97%; the remainder is a handful
of defensively-unreachable branches and allocation-failure paths that are
only reachable at a specific reallocation boundary.

To embed the engine in another program, compile `regex.c` with neither
`RX_TEST` nor `RX_MAIN` defined and link it in. The public interface is
the declaration block at the top of `regex.c`; copy it into a `regex.h`
if you prefer a separate header.

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
#define RX_ICASE     0x01
#define RX_MULTILINE 0x02
#define RX_DOTALL    0x04
#define RX_GLOBAL    0x08

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
char *rx_replace(rx_t *re, const char *text, size_t len,
                 const char *repl, int flags);
```

`rx_exec` returns 1 on a match, 0 when none is found, or -1 on error.
Entry 0 of `m` is the whole match; entries 1..`rx_ngroups` are the
captured groups. `rx_replace` returns a freshly allocated,
NUL-terminated string that the caller frees.

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
