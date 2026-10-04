/* torture.c : heavy correctness and robustness tests for rx.c */
/*
 * Build via the Makefile `torture` / `asan` / `ubsan` / `cov` targets.
 * Includes rx.c directly so coverage and the sanitizers see the whole
 * engine as one translation unit.
 *
 * The tests are small single-concern functions. Each section driver
 * (battery, robustness, editor_api, api_edge, fault_suite) is just a
 * list of those functions, and main() runs the drivers in order.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Fault injection. Every allocation inside rx.c is routed through a
 * hook that can fail the Nth allocation on demand. The hook is disarmed
 * by default, so it is a zero-cost passthrough until a sweep arms it.
 * The system headers are all included above first, so the macros below
 * only ever rewrite allocation calls in rx.c's own code, never a
 * header declaration. free() is left alone.
 */
static long rx_fi_count;    /* allocations seen while armed */
static long rx_fi_fail_at;  /* fail this counted allocation; 0 disables */
static int  rx_fi_armed;    /* only count/fail while set */

static int
rx_fi_trip(void)
{
    if (!rx_fi_armed)
        return 0;
    rx_fi_count++;
    return rx_fi_fail_at && rx_fi_count == rx_fi_fail_at;
}

static void *
rx_fi_malloc(size_t n)
{
    return rx_fi_trip() ? NULL : malloc(n);
}

static void *
rx_fi_realloc(void *p, size_t n)
{
    return rx_fi_trip() ? NULL : realloc(p, n);
}

static void *
rx_fi_calloc(size_t a, size_t b)
{
    return rx_fi_trip() ? NULL : calloc(a, b);
}

#define malloc  rx_fi_malloc
#define realloc rx_fi_realloc
#define calloc  rx_fi_calloc

#include "../rx.c"

static int tests, fails;

/****************************************************************
 * Assertion helpers
 ****************************************************************/

/* Compile and search `s`; assert the 1/0/-1 result equals `want`. */
static void
ck_match(const char *pat, const char *s, int flags, int want)
{
    const char *err;
    rx_t *re = rx_compile(pat, flags, &err);
    rx_match m[10];
    int got;

    tests++;
    if (!re) {
        fails++;
        printf("FAIL compile /%s/: %s\n", pat, err);
        return;
    }
    got = rx_exec(re, s, strlen(s), 0, m, 10);
    rx_free(re);
    if (got != want) {
        fails++;
        printf("FAIL match /%s/ on \"%s\": got %d want %d\n",
               pat, s, got, want);
    }
}

/* Check that a pattern fails to compile. */
static void
ck_badpat(const char *pat)
{
    const char *err = NULL;
    rx_t *re = rx_compile(pat, 0, &err);

    tests++;
    if (re) {
        fails++;
        printf("FAIL /%s/ should not compile\n", pat);
        rx_free(re);
    } else if (!err) {
        fails++;
        printf("FAIL /%s/ failed without an error string\n", pat);
    }
}

/* Check a specific captured group's text. */
static void
ck_group(const char *pat, const char *s, int flags, int g,
         const char *want)
{
    const char *err;
    rx_t *re = rx_compile(pat, flags, &err);
    rx_match m[10];

    tests++;
    if (!re) {
        fails++;
        printf("FAIL compile /%s/: %s\n", pat, err);
        return;
    }
    if (rx_exec(re, s, strlen(s), 0, m, 10) != 1) {
        fails++;
        printf("FAIL /%s/ on \"%s\" did not match\n", pat, s);
        rx_free(re);
        return;
    }
    if (m[g].so < 0 || m[g].eo < 0) {
        fails++;
        printf("FAIL /%s/ group %d unset, want \"%s\"\n", pat, g, want);
    } else {
        long n = m[g].eo - m[g].so;

        if ((long)strlen(want) != n ||
            memcmp(want, s + m[g].so, (size_t)n) != 0) {
            fails++;
            printf("FAIL /%s/ group %d: got \"%.*s\" want \"%s\"\n",
                   pat, g, (int)n, s + m[g].so, want);
        }
    }
    rx_free(re);
}

/* Compile and replace; assert the result string equals `want`. */
static void
ck_sub(const char *pat, const char *s, const char *repl, int flags,
       const char *want)
{
    const char *err;
    rx_t *re = rx_compile(pat, flags, &err);
    char *got;

    tests++;
    if (!re) {
        fails++;
        printf("FAIL compile /%s/: %s\n", pat, err);
        return;
    }
    got = rx_replace(re, s, strlen(s), repl, flags);
    rx_free(re);
    if (!got || strcmp(got, want) != 0) {
        fails++;
        printf("FAIL sub /%s/ \"%s\" ~ \"%s\": got \"%s\" want \"%s\"\n",
               pat, s, repl, got ? got : "(null)", want);
    }
    free(got);
}

/* Assert rx_matches_newline for a compiled pattern. */
static void
ck_nl(const char *pat, int flags, int want)
{
    const char *err;
    rx_t *re = rx_compile(pat, flags, &err);

    tests++;
    if (!re || rx_matches_newline(re) != want) {
        fails++;
        printf("FAIL matches_newline /%s/: want %d\n", pat, want);
    }
    rx_free(re);
}

/* Assert rx_search finds a match starting at `wso`, or no match if wso<0. */
static void
ck_search(const char *pat, const char *s, size_t from, int sflags, long wso)
{
    const char *err;
    rx_t *re = rx_compile(pat, 0, &err);
    rx_match m[1];
    int r;

    tests++;
    if (!re) {
        fails++;
        printf("FAIL compile /%s/: %s\n", pat, err);
        return;
    }
    r = rx_search(re, s, strlen(s), from, sflags, m, 1);
    if (wso < 0) {
        if (r != 0) {
            fails++;
            printf("FAIL search /%s/ from %zu sf=%#x: want miss, got %d\n",
                   pat, from, sflags, r);
        }
    } else if (r != 1 || m[0].so != wso) {
        fails++;
        printf("FAIL search /%s/ from %zu sf=%#x: want so=%ld, got r=%d so=%ld\n",
               pat, from, sflags, wso, r, r == 1 ? m[0].so : -1);
    }
    rx_free(re);
}

/* Assert rx_search returns an error (-1). */
static void
ck_search_err(const char *pat, const char *s, size_t from, int sflags)
{
    const char *err;
    rx_t *re = rx_compile(pat, 0, &err);
    rx_match m[1];

    tests++;
    if (!re || rx_search(re, s, strlen(s), from, sflags, m, 1) != -1) {
        fails++;
        printf("FAIL search error /%s/ from %zu sf=%#x\n", pat, from, sflags);
    }
    rx_free(re);
}

/* Allocate `n` copies of `ch` plus a NUL terminator. */
static char *
alloc_filled(size_t n, int ch)
{
    char *b = malloc(n + 1);

    if (b) {
        memset(b, ch, n);
        b[n] = '\0';
    }
    return b;
}

/* Fill `buf` (>= 42 bytes) with the classic catastrophic-backtracking
 * subject: 40 'a's followed by a non-'a' so "(a+)+$" cannot match. */
static void
make_catastrophic(char *buf)
{
    memset(buf, 'a', 40);
    buf[40] = 'X';
    buf[41] = '\0';
}

/****************************************************************
 * Correctness battery
 ****************************************************************/

static void
t_empty_patterns(void)
{
    ck_match("", "", 0, 1);
    ck_match("", "abc", 0, 1);
    ck_match("^$", "", 0, 1);
    ck_match("^$", "x", 0, 0);
    ck_match("()", "x", 0, 1);
    ck_match("(?:)", "x", 0, 1);
    ck_match("a{0}", "b", 0, 1);        /* matches empty */
    ck_match("a{0,0}b", "b", 0, 1);
}

static void
t_quantifier_bounds(void)
{
    ck_match("^a{2,4}$", "a", 0, 0);
    ck_match("^a{2,4}$", "aa", 0, 1);
    ck_match("^a{2,4}$", "aaaa", 0, 1);
    ck_match("^a{2,4}$", "aaaaa", 0, 0);
    ck_match("^a{3}$", "aaa", 0, 1);
    ck_match("^(ab){2,3}$", "ababab", 0, 1);
    ck_match("^(ab){2,3}$", "abababab", 0, 0);
}

/* Nullable repetition must terminate and match, not loop (F1). */
static void
t_nullable_repetition(void)
{
    ck_match("^(a*)*$", "aaa", 0, 1);
    ck_group("^(a*)*$", "aaaaa", 0, 1, "");             /* last iter empty */
    ck_match("^(a?)*$", "aaa", 0, 1);
    ck_match("(a?)*b", "b", 0, 1);
    ck_match("^(a*)+$", "aa", 0, 1);
    ck_match("^()*$", "", 0, 1);
    ck_match("(?:)*", "x", 0, 1);
    ck_match("^(a|)*$", "aaa", 0, 1);                   /* nullable branch */
    ck_match("^(|a)*$", "aaa", 0, 1);
    ck_match("^(a*|b*)*$", "aabbb", 0, 1);
    ck_match("x(.+)+y", "xabcy", 0, 1);                 /* progressing nest */
}

/* Repetition counts are bounded (F2): a huge or overflowing count is
 * rejected rather than expanded or overflowed. */
static void
t_repetition_counts(void)
{
    ck_badpat("a{40000}");
    ck_badpat("a{0,40000}");
    ck_badpat("a{99999999999}");                        /* would overflow int */
    ck_badpat("a{2,99999999999}");
    ck_match("a{32767}", "a", 0, 0);                     /* at the cap: valid */
}

static void
t_greedy_lazy(void)
{
    ck_group("a(.*)c", "axxcyyc", 0, 1, "xxcyy");       /* greedy */
    ck_group("a(.*?)c", "axxcyyc", 0, 1, "xx");         /* lazy    */
    ck_group("(a+)(a+)", "aaaa", 0, 1, "aaa");          /* greedy first */
    ck_group("(a+?)(a+)", "aaaa", 0, 1, "a");           /* lazy first  */
}

/* Alternation precedence (leftmost alternative preferred). */
static void
t_alternation(void)
{
    ck_group("(a|ab)", "ab", 0, 1, "a");
    ck_match("^(a|ab)c$", "abc", 0, 1);                 /* must backtrack */
    ck_match("foo|bar|baz", "xbazy", 0, 1);
    ck_match("^(cat|dog|fish)$", "dog", 0, 1);
}

static void
t_backreferences(void)
{
    ck_group("((a)(b))", "ab", 0, 2, "a");
    ck_group("((a)(b))", "ab", 0, 3, "b");
    ck_match("(a+)b\\1", "aabaa", 0, 1);
    ck_match("(a+)b\\1", "aabaaa", 0, 1);               /* \1 = "aa" */
    ck_match("^(a+)b\\1$", "aabaaa", 0, 0);             /* anchored, no */
    ck_match("(['\"]).*?\\1", "say 'hi' there", 0, 1);  /* quote match */
    ck_match("(a)(b)?c\\2", "ac", 0, 1);                /* unset \2 empty */
}

/* Classes: ranges, negation, POSIX, escapes inside brackets. */
static void
t_bracket_classes(void)
{
    ck_match("^[a-fA-F0-9]+$", "DeadBeef00", 0, 1);
    ck_match("[^0-9]", "12345", 0, 0);
    ck_match("[]]", "]", 0, 1);                         /* ] as first char */
    ck_match("[a\\]b]+", "a]b", 0, 1);                  /* escaped ] */
    ck_match("[\\t]", "\t", 0, 1);
    ck_match("[[:space:][:digit:]]+", " 7\t9", 0, 1);
    ck_match("[-a]", "-", 0, 1);                        /* leading dash */
    ck_match("[a-]", "-", 0, 1);                        /* trailing dash */
    ck_badpat("[a-\\");                                 /* range hi is a
                                                         * trailing backslash */
    ck_badpat("[z-a]");                                 /* reversed range */
}

/* Shorthand classes and their negations. */
static void
t_shorthand_classes(void)
{
    ck_match("^\\d+\\.\\d+$", "3.14", 0, 1);
    ck_match("\\D", "7", 0, 0);
    ck_match("\\W", "_", 0, 0);
    ck_match("\\W", "!", 0, 1);
    ck_match("\\S+", "   ", 0, 0);
    ck_match("\\bword\\b", ".word.", 0, 1);
    ck_match("\\Bin\\B", "pointing", 0, 1);
    ck_match("\\<the\\>", "the end", 0, 1);
    ck_match("\\<the\\>", "theory", 0, 0);
}

/* Shorthand classes inside bracket expressions (F3): the whole
 * membership joins the set, it is not read as a literal letter. */
static void
t_shorthand_in_brackets(void)
{
    ck_match("^[\\d]+$", "0123", 0, 1);
    ck_match("[\\d]", "d", 0, 0);                        /* not literal 'd' */
    ck_match("^[\\w]+$", "foo_1", 0, 1);
    ck_match("[\\w]", "!", 0, 0);
    ck_match("^[\\s]+$", " \t\n", 0, 1);
    ck_match("[\\D]", "5", 0, 0);                        /* negated form */
    ck_match("^[\\D]+$", "abc.", 0, 1);
    ck_match("[\\W]", "_", 0, 0);
    ck_match("[\\S]", " ", 0, 0);
    ck_match("^[a\\d]+$", "a7a", 0, 1);                  /* mixed with literal */
    ck_match("^[\\d\\s]+$", "1 2\t3", 0, 1);             /* two shorthands */
    ck_match("^[x\\dy]+$", "x5y", 0, 1);                 /* shorthand mid-set */
}

static void
t_anchors_multiline(void)
{
    ck_match("^b", "a\nb", RX_MULTILINE, 1);
    ck_match("^b", "a\nb", 0, 0);
    ck_match("c$", "c\nd", RX_MULTILINE, 1);
    ck_match(".", "\n", 0, 0);
    ck_match(".", "\n", RX_DOTALL, 1);
}

static void
t_case_folding(void)
{
    ck_match("^[a-z]+$", "AbCdEf", RX_ICASE, 1);
    ck_match("^[^a-z]+$", "AbCdEf", RX_ICASE, 0);       /* negation + icase */
    ck_match("(x)\\1", "xX", RX_ICASE, 1);              /* icase backref */
}

static void
t_escapes(void)
{
    ck_match("\\x41\\x42", "AB", 0, 1);
    ck_match("a\\tb", "a\tb", 0, 1);
    ck_match("\\x4", "\x04", 0, 1);                     /* one hex digit */
    ck_match("\\x6a", "j", 0, 1);                       /* lowercase hex */
    ck_match("\\x4A", "J", 0, 1);                       /* uppercase hex */
    ck_match("\\xz", "xz", 0, 1);                       /* lone \x literal */
    ck_match("\\n\\r\\f\\v\\a", "\n\r\f\v\a", 0, 1);    /* control escapes */
}

static void
t_interval_forms(void)
{
    ck_match("^a{2,}$", "aaa", 0, 1);                   /* unbounded upper */
    ck_match("^a{2,}$", "a", 0, 0);
    ck_match("a{2,x}", "a{2,x}", 0, 1);                 /* malformed, literal */
    ck_match("a{2z}", "a{2z}", 0, 1);                   /* malformed, literal */
}

static void
t_literal_metachars(void)
{
    ck_match("a\\.c", "a.c", 0, 1);
    ck_match("a\\.c", "abc", 0, 0);
    ck_match("\\(\\)", "()", 0, 1);
    ck_match("a\\+", "a+", 0, 1);
    ck_match("100\\$", "100$", 0, 1);
    ck_match("a{", "a{", 0, 1);                         /* bare { literal */
    ck_match("a{x}", "a{x}", 0, 1);                     /* invalid interval */
}

/* Invalid patterns must be rejected, not crash. */
static void
t_invalid_patterns(void)
{
    ck_badpat("(");
    ck_badpat(")");
    ck_badpat("a)");
    ck_badpat("[a");
    ck_badpat("[a-");
    ck_badpat("*");
    ck_badpat("+a");
    ck_badpat("a\\");
    ck_badpat("\\1");                                   /* no such group */
    ck_badpat("(a)\\2");
    ck_badpat("a{2,1}");
    ck_badpat("[z-a]");
    ck_badpat("[[:bogus:]]");
    ck_badpat("[[:alpha]");                             /* unterminated [: */
    ck_badpat("[\\");                                   /* trailing \ in [ */
}

/* The \0 escape compiles to a NUL byte matcher. */
static void
t_nul_byte(void)
{
    ck_match("\\0", "", 0, 0);
}

static void
t_substitution(void)
{
    ck_sub("", "abc", "-", RX_GLOBAL, "-a-b-c-");
    ck_sub("$", "abc", "!", 0, "abc!");
    ck_sub("^", "abc", ">", 0, ">abc");
    ck_sub("(\\w+) (\\w+)", "hello world", "\\2 \\1", 0, "world hello");
    ck_sub("[aeiou]", "regular", "_", RX_GLOBAL, "r_g_l_r");
    ck_sub("\\w+", "hi there", "\\U&\\E!", RX_GLOBAL, "HI! THERE!");
    ck_sub("(\\w)(\\w*)", "mixED", "\\l\\1\\U\\2", 0, "mIXED");
    ck_sub("x", "abc", "\\9", RX_GLOBAL, "abc");        /* no group 9 */
    ck_sub("a", "a", "\\", 0, "\\");                    /* trailing bslash */
    ck_sub("a", "a", "b\\u", 0, "b");                   /* dangling \\u */
    ck_sub("o+", "foo", "0", 0, "f0");
    ck_sub("l", "hello", "L", 0, "heLlo");              /* first only */
    ck_sub("[0-9]+", "a1b22c333", "#", RX_GLOBAL, "a#b#c#");
    ck_sub("\\&", "a&b", "and", 0, "aandb");            /* match literal & */
    ck_sub("x", "x", "\\&", 0, "&");                    /* literal & in repl */
    ck_sub("x", "x", "\\z", 0, "z");                    /* unknown \\ escape */
    ck_sub("(\\w)", "abc", "\\u\\1", RX_GLOBAL, "ABC"); /* \\u one-shot */
    ck_sub("(\\w)", "ABC", "\\l\\1", RX_GLOBAL, "abc"); /* \\l one-shot */
    ck_sub("(\\w+)", "HELLO", "\\L\\1", 0, "hello");    /* \\L sticky */
    ck_sub("x", "x", "\\n\\t\\r\\f\\v\\a\\\\", 0,       /* repl escapes */
           "\n\t\r\f\v\a\\");
    ck_sub("x", "x", "\\x41\\x42", 0, "AB");            /* hex escape */
    ck_sub("x", "x", "\\x6a", 0, "j");                  /* lowercase hex */
    ck_sub("x", "x", "\\x4", 0, "\x04");                /* one hex digit */
    ck_sub("x", "x", "\\xg", 0, "xg");                  /* lone \\x literal */
    ck_sub("x", "x", "\\x", 0, "x");                    /* trailing \\x */
    ck_sub("a", "a", "", 0, "");                        /* empty result */
}

/* Parser recursion is bounded (F4): a pattern nested past the depth limit
 * is a clean compile error, not a stack overflow. A deep but legal nest
 * still compiles. */
static void
t_deep_nesting(void)
{
    char deep[9000];
    int i, n;

    n = 0;
    for (i = 0; i < 4000; i++)
        deep[n++] = '(';
    deep[n++] = 'a';
    for (i = 0; i < 4000; i++)
        deep[n++] = ')';
    deep[n] = '\0';
    ck_badpat(deep);

    n = 0;
    for (i = 0; i < 500; i++)
        deep[n++] = '(';
    deep[n++] = 'a';
    for (i = 0; i < 500; i++)
        deep[n++] = ')';
    deep[n] = '\0';
    ck_match(deep, "a", 0, 1);                          /* legal deep nest */
}

static void
battery(void)
{
    t_empty_patterns();
    t_quantifier_bounds();
    t_nullable_repetition();
    t_repetition_counts();
    t_greedy_lazy();
    t_alternation();
    t_backreferences();
    t_bracket_classes();
    t_shorthand_classes();
    t_shorthand_in_brackets();
    t_anchors_multiline();
    t_case_folding();
    t_escapes();
    t_interval_forms();
    t_literal_metachars();
    t_invalid_patterns();
    t_nul_byte();
    t_substitution();
    t_deep_nesting();
}

/****************************************************************
 * Robustness: long inputs and catastrophic backtracking
 ****************************************************************/

/* A long greedy match must not overflow the native stack. */
static void
t_long_greedy(void)
{
    const char *err;
    size_t big = 200000;
    char *buf = alloc_filled(big, 'a');
    rx_t *re = rx_compile(".*", 0, &err);
    rx_match m[1];

    tests++;
    if (!re || !buf || rx_exec(re, buf, big, 0, m, 1) != 1 ||
        m[0].eo != (long)big) {
        fails++;
        printf("FAIL long greedy .* match\n");
    }
    rx_free(re);
    free(buf);
}

static void
t_long_backref(void)
{
    const char *err;
    size_t big = 200000;
    char *buf = alloc_filled(big, 'a');
    rx_t *re = rx_compile("(a+)\\1", 0, &err);
    rx_match m[2];

    tests++;
    if (!re || !buf || rx_exec(re, buf, big, 0, m, 2) != 1) {
        fails++;
        printf("FAIL long backreference\n");
    }
    rx_free(re);
    free(buf);
}

/* A catastrophic pattern must hit the step budget and return an error
 * rather than hang. */
static void
t_catastrophic(void)
{
    char bad[64];

    make_catastrophic(bad);
    ck_match("(a+)+$", bad, 0, -1);
}

/* nmatch of 0 with a NULL match array must be safe. */
static void
t_nmatch_zero(void)
{
    const char *err;
    rx_t *re = rx_compile("abc", 0, &err);

    tests++;
    if (!re || rx_exec(re, "zabc", 4, 0, NULL, 0) != 1) {
        fails++;
        printf("FAIL exec with nmatch 0\n");
    }
    rx_free(re);
}

/* A match far into a long subject makes rx_replace copy a prefix much
 * larger than the output buffer's first growth step, exercising the
 * repeated-doubling path in the string builder. */
static void
t_long_substitution(void)
{
    const char *err;
    size_t big = 200000;
    char *buf = alloc_filled(big, 'a');
    rx_t *re = rx_compile("Z", 0, &err);
    char *out;

    if (buf)
        buf[big - 1] = 'Z';
    out = (re && buf) ? rx_replace(re, buf, big, "!", 0) : NULL;
    tests++;
    if (!out || strlen(out) != big || out[big - 1] != '!') {
        fails++;
        printf("FAIL long-prefix substitution\n");
    }
    free(out);
    rx_free(re);
    free(buf);
}

static void
robustness(void)
{
    t_long_greedy();
    t_long_backref();
    t_catastrophic();
    t_nmatch_zero();
    t_long_substitution();
}

/****************************************************************
 * Public-API edges and editor-oriented helpers
 ****************************************************************/

static void
t_group_count(void)
{
    const char *err;
    rx_t *re = rx_compile("(a)(b)(c)", 0, &err);

    tests++;
    if (!re || rx_ngroups(re) != 3) {
        fails++;
        printf("FAIL rx_ngroups\n");
    }
    rx_free(re);
}

static void
t_null_safe(void)
{
    tests++;
    if (rx_ngroups(NULL) != 0 ||
        rx_exec(NULL, "x", 1, 0, NULL, 0) != -1 ||
        rx_replace(NULL, "x", 1, "y", 0) != NULL ||
        rx_matches_newline(NULL) != 0 ||
        rx_search(NULL, "x", 1, 0, 0, NULL, 0) != -1) {
        fails++;
        printf("FAIL NULL-safe entry points\n");
    }
    rx_free(NULL);      /* must be a no-op */
}

static void
api_edge(void)
{
    t_group_count();
    t_null_safe();
}

/* A match is single-line unless it can contain a newline byte. */
static void
t_newline_span(void)
{
    ck_nl("abc", 0, 0);
    ck_nl("a.c", 0, 0);                 /* dot excludes newline       */
    ck_nl("a.c", RX_DOTALL, 1);         /* unless DOTALL              */
    ck_nl("x\\ny", 0, 1);               /* literal newline            */
    ck_nl("[\\n]", 0, 1);
    ck_nl("\\s", 0, 1);                 /* shorthand includes newline */
    ck_nl("\\d", 0, 0);
    ck_nl("[^a]", 0, 1);                /* negated class includes it  */
    ck_nl("^foo$", RX_MULTILINE, 0);    /* anchors are zero-width     */
    ck_nl("(a)b\\1", 0, 0);             /* backref of newline-free group */
    ck_nl("(\\n)\\1", 0, 1);
}

/* Forward, backward, and wrap-around on three matches at 0, 8, 16. */
static void
t_search_directions(void)
{
    const char *t = "foo bar foo baz foo";

    ck_search("foo", t, 0, 0, 0);
    ck_search("foo", t, 1, 0, 8);
    ck_search("foo", t, 9, 0, 16);
    ck_search("foo", t, 17, 0, -1);                 /* nothing after */
    ck_search("foo", t, 17, RX_WRAP, 0);            /* wraps to top  */
    ck_search("foo", t, 10, RX_BACKWARD, 8);        /* last before 10 */
    ck_search("foo", t, 8, RX_BACKWARD, 0);         /* strictly before */
    ck_search("foo", t, 0, RX_BACKWARD, -1);        /* nothing before */
    ck_search("foo", t, 0, RX_BACKWARD | RX_WRAP, 16); /* wraps to last */
    ck_search("zzz", t, 0, RX_WRAP, -1);            /* no match anywhere */
}

/* A matcher error (step budget) during a backward search propagates as
 * -1 rather than being mistaken for "no match". */
static void
t_search_error(void)
{
    char bad[64];

    make_catastrophic(bad);
    ck_search_err("(a+)+$", bad, strlen(bad), RX_BACKWARD);
}

static void
editor_api(void)
{
    t_newline_span();
    t_search_directions();
    t_search_error();
}

/****************************************************************
 * Fault injection: exhaustive single-allocation-failure sweep.
 *
 * For each operation we first run it once, armed but failing nothing, to
 * count how many allocations it makes. We then run it again once per
 * allocation, failing exactly that one, and assert the documented
 * failure value is returned. Run under AddressSanitizer (make fault),
 * the leak detector proves every partial allocation is released on the
 * error path.
 ****************************************************************/

static int fault_checks;

static long
fi_count(void)
{
    rx_fi_count = 0;
    rx_fi_fail_at = 0;      /* count only, fail nothing */
    rx_fi_armed = 1;
    return 0;               /* caller reads rx_fi_count after the op */
}

static void
fi_arm(long at)
{
    rx_fi_count = 0;
    rx_fi_fail_at = at;
    rx_fi_armed = 1;
}

static void
fi_disarm(void)
{
    rx_fi_armed = 0;
}

static void
sweep_compile(const char *pat)
{
    const char *err;
    rx_t *re;
    long k, i;

    fi_count();
    re = rx_compile(pat, 0, &err);
    fi_disarm();
    k = rx_fi_count;
    if (re)
        rx_free(re);

    for (i = 1; i <= k; i++) {
        fi_arm(i);
        re = rx_compile(pat, 0, &err);
        fi_disarm();
        tests++;
        fault_checks++;
        if (re) {
            fails++;
            printf("FAIL fault compile /%s/ @%ld: returned non-NULL\n",
                   pat, i);
            rx_free(re);
        }
    }
}

static void
sweep_exec(const char *pat, const char *s)
{
    const char *err;
    rx_t *re = rx_compile(pat, 0, &err);
    rx_match m[10];
    size_t len = strlen(s);
    long k, i;

    tests++;
    if (!re) {
        fails++;
        printf("FAIL fault-exec setup /%s/: %s\n", pat, err);
        return;
    }

    fi_count();
    rx_exec(re, s, len, 0, m, 10);
    fi_disarm();
    k = rx_fi_count;

    for (i = 1; i <= k; i++) {
        int r;

        fi_arm(i);
        r = rx_exec(re, s, len, 0, m, 10);
        fi_disarm();
        tests++;
        fault_checks++;
        if (r != -1) {
            fails++;
            printf("FAIL fault exec /%s/ on \"%s\" @%ld: got %d want -1\n",
                   pat, s, i, r);
        }
    }
    rx_free(re);
}

static void
sweep_replace(const char *pat, const char *s, const char *repl, int flags)
{
    const char *err;
    rx_t *re = rx_compile(pat, 0, &err);
    size_t len = strlen(s);
    char *out;
    long k, i;

    tests++;
    if (!re) {
        fails++;
        printf("FAIL fault-replace setup /%s/: %s\n", pat, err);
        return;
    }

    fi_count();
    out = rx_replace(re, s, len, repl, flags);
    fi_disarm();
    k = rx_fi_count;
    free(out);

    for (i = 1; i <= k; i++) {
        fi_arm(i);
        out = rx_replace(re, s, len, repl, flags);
        fi_disarm();
        tests++;
        fault_checks++;
        if (out) {
            fails++;
            printf("FAIL fault replace /%s/ @%ld: returned non-NULL\n",
                   pat, i);
            free(out);
        }
    }
    rx_free(re);
}

static void
fault_suite(void)
{
    static char longa[128];
    int i;

    for (i = 0; i < (int)sizeof longa - 1; i++)
        longa[i] = 'a';
    longa[sizeof longa - 1] = '\0';

    /* Compile: patterns that touch every allocation site (program growth,
     * class bitmaps and the set list, quantifier templates, groups). */
    sweep_compile("a");
    sweep_compile("abcdefghijklmnop");          /* program realloc */
    sweep_compile("[a-z0-9_]+");                 /* class set + list */
    sweep_compile("[[:alpha:][:digit:]]");       /* multiple sets */
    sweep_compile("(ab){2,5}c");                 /* interval templates */
    sweep_compile("(a|bb|ccc)+d");               /* alternation templates */
    sweep_compile("(\\w+)@(\\w+)\\.(\\w+)");     /* groups + shorthand */
    sweep_compile("((a)(b)(c))*");               /* nested group saves */

    /* Exec: patterns that grow the choice and undo stacks. */
    sweep_exec("abc", "zzabc");
    sweep_exec("a.*b.*c", "axxbyyczz");
    sweep_exec("(a|bb|ccc)+z", "abbcccabb");
    sweep_exec("(a)*", longa);                   /* undo + choice growth */
    sweep_exec(".*x", longa);                    /* deep choice stack */
    sweep_exec("(\\w+)@(\\w+)", "user@host");

    /* Replace: output-buffer growth and the empty-match path. */
    sweep_replace("", "abcde", "-", RX_GLOBAL);
    sweep_replace("[aeiou]", "education", "_", RX_GLOBAL);
    sweep_replace("(\\w+)", "one two three", "[\\1]", RX_GLOBAL);
    sweep_replace("a", longa, "bb", RX_GLOBAL);  /* sbuf doubling */
    sweep_replace("z", "abc", "y", RX_GLOBAL);   /* no match, tail only */

    printf("fault: %d single-allocation-failure checks\n", fault_checks);
}

/****************************************************************
 * Deterministic fuzzer: no oracle, just drive the engine on random
 * patterns and strings so the sanitizers exercise every path without a
 * crash, leak, or undefined operation.
 ****************************************************************/

static unsigned long prng = 0x9e3779b97f4a7c15UL;

static unsigned
rnd(unsigned n)
{
    prng = prng * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)((prng >> 33) % n);
}

static void
gen_pattern(char *out, size_t cap)
{
    static const char *tok[] = {
        "a", "b", "c", ".", "*", "+", "?", "(", ")", "|", "[", "]",
        "^", "$", "\\d", "\\w", "\\s", "\\b", "[a-c]", "[^a]",
        "{1,2}", "\\1", "(?:", "x", "-", "{", "}", "\\.",
    };
    size_t len = 0;
    int n = 1 + (int)rnd(10);
    int i;

    for (i = 0; i < n; i++) {
        const char *t = tok[rnd(sizeof tok / sizeof tok[0])];
        size_t tl = strlen(t);

        if (len + tl + 1 >= cap)
            break;
        memcpy(out + len, t, tl);
        len += tl;
    }
    out[len] = '\0';
}

static void
gen_text(char *out, size_t cap)
{
    static const char alpha[] = "abcABC123 \t\n-.";
    size_t n = rnd((unsigned)cap);
    size_t i;

    for (i = 0; i < n; i++)
        out[i] = alpha[rnd(sizeof alpha - 1)];
    out[n] = '\0';
}

static void
fuzz(int iters)
{
    int i;
    int compiled = 0, matched = 0;

    for (i = 0; i < iters; i++) {
        char pat[64], txt[64];
        const char *err;
        rx_t *re;
        int flags = (int)rnd(16);       /* random mix of ICASE/ML/DOTALL/GLOBAL */

        gen_pattern(pat, sizeof pat);
        gen_text(txt, sizeof txt);

        re = rx_compile(pat, flags & (RX_ICASE | RX_MULTILINE | RX_DOTALL),
                        &err);
        if (!re)
            continue;
        compiled++;
        {
            rx_match m[10];
            int r = rx_exec(re, txt, strlen(txt), 0, m, 10);
            char *s;

            if (r == 1)
                matched++;
            s = rx_replace(re, txt, strlen(txt), "<\\0>", flags);
            free(s);
            s = rx_replace(re, txt, strlen(txt), "\\U\\1\\E&", flags);
            free(s);
        }
        rx_free(re);
    }
    printf("fuzz: %d iterations, %d compiled, %d matched\n",
           iters, compiled, matched);
}

int
main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 20000;

    battery();
    robustness();
    api_edge();
    editor_api();
    fault_suite();
    fuzz(iters);        /* iters == 0 skips the fuzzer */

    printf("%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
