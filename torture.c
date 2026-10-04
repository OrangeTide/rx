/* torture.c : heavy correctness and robustness tests for regex.c */
/*
 * Build via the Makefile `torture` / `asan` / `ubsan` / `cov` targets.
 * Includes regex.c directly so coverage and the sanitizers see the whole
 * engine as one translation unit.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Fault injection. Every allocation inside regex.c is routed through a
 * hook that can fail the Nth allocation on demand. The hook is disarmed
 * by default, so it is a zero-cost passthrough until a sweep arms it.
 * The system headers are all included above first, so the macros below
 * only ever rewrite allocation calls in regex.c's own code, never a
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

#include "regex.c"

static int tests, fails;

/****************************************************************
 * Assertion helpers
 ****************************************************************/

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

/****************************************************************
 * Hand-verified battery
 ****************************************************************/

static void
battery(void)
{
    /* Empty and degenerate patterns. */
    ck_match("", "", 0, 1);
    ck_match("", "abc", 0, 1);
    ck_match("^$", "", 0, 1);
    ck_match("^$", "x", 0, 0);
    ck_match("()", "x", 0, 1);
    ck_match("(?:)", "x", 0, 1);
    ck_match("a{0}", "b", 0, 1);        /* matches empty */
    ck_match("a{0,0}b", "b", 0, 1);

    /* Quantifier boundaries. */
    ck_match("^a{2,4}$", "a", 0, 0);
    ck_match("^a{2,4}$", "aa", 0, 1);
    ck_match("^a{2,4}$", "aaaa", 0, 1);
    ck_match("^a{2,4}$", "aaaaa", 0, 0);
    ck_match("^a{3}$", "aaa", 0, 1);
    ck_match("^(ab){2,3}$", "ababab", 0, 1);
    ck_match("^(ab){2,3}$", "abababab", 0, 0);

    /* Nullable repetition must terminate and match, not loop (F1). */
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

    /* Repetition counts are bounded (F2): a huge or overflowing count is
     * rejected rather than expanded or overflowed. */
    ck_badpat("a{40000}");
    ck_badpat("a{0,40000}");
    ck_badpat("a{99999999999}");                        /* would overflow int */
    ck_badpat("a{2,99999999999}");
    ck_match("a{32767}", "a", 0, 0);                     /* at the cap: valid */

    /* Greedy vs lazy capture extent. */
    ck_group("a(.*)c", "axxcyyc", 0, 1, "xxcyy");       /* greedy */
    ck_group("a(.*?)c", "axxcyyc", 0, 1, "xx");         /* lazy    */
    ck_group("(a+)(a+)", "aaaa", 0, 1, "aaa");          /* greedy first */
    ck_group("(a+?)(a+)", "aaaa", 0, 1, "a");           /* lazy first  */

    /* Alternation precedence (leftmost alternative preferred). */
    ck_group("(a|ab)", "ab", 0, 1, "a");
    ck_match("^(a|ab)c$", "abc", 0, 1);                 /* must backtrack */
    ck_match("foo|bar|baz", "xbazy", 0, 1);
    ck_match("^(cat|dog|fish)$", "dog", 0, 1);

    /* Nested groups and backreferences. */
    ck_group("((a)(b))", "ab", 0, 2, "a");
    ck_group("((a)(b))", "ab", 0, 3, "b");
    ck_match("(a+)b\\1", "aabaa", 0, 1);
    ck_match("(a+)b\\1", "aabaaa", 0, 1);               /* \1 = "aa" */
    ck_match("^(a+)b\\1$", "aabaaa", 0, 0);             /* anchored, no */
    ck_match("(['\"]).*?\\1", "say 'hi' there", 0, 1);  /* quote match */
    ck_match("(a)(b)?c\\2", "ac", 0, 1);                /* unset \2 empty */

    /* Classes: ranges, negation, POSIX, escapes inside brackets. */
    ck_match("^[a-fA-F0-9]+$", "DeadBeef00", 0, 1);
    ck_match("[^0-9]", "12345", 0, 0);
    ck_match("[]]", "]", 0, 1);                         /* ] as first char */
    ck_match("[a\\]b]+", "a]b", 0, 1);                  /* escaped ] */
    ck_match("[\\t]", "\t", 0, 1);
    ck_match("[[:space:][:digit:]]+", " 7\t9", 0, 1);
    ck_match("[-a]", "-", 0, 1);                        /* leading dash */
    ck_match("[a-]", "-", 0, 1);                        /* trailing dash */

    /* Shorthand classes and their negations. */
    ck_match("^\\d+\\.\\d+$", "3.14", 0, 1);
    ck_match("\\D", "7", 0, 0);
    ck_match("\\W", "_", 0, 0);
    ck_match("\\W", "!", 0, 1);
    ck_match("\\S+", "   ", 0, 0);
    ck_match("\\bword\\b", ".word.", 0, 1);
    ck_match("\\Bin\\B", "pointing", 0, 1);
    ck_match("\\<the\\>", "the end", 0, 1);
    ck_match("\\<the\\>", "theory", 0, 0);

    /* Shorthand classes inside bracket expressions (F3): the whole
     * membership joins the set, it is not read as a literal letter. */
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

    /* Anchors and multiline. */
    ck_match("^b", "a\nb", RX_MULTILINE, 1);
    ck_match("^b", "a\nb", 0, 0);
    ck_match("c$", "c\nd", RX_MULTILINE, 1);
    ck_match(".", "\n", 0, 0);
    ck_match(".", "\n", RX_DOTALL, 1);

    /* Case folding. */
    ck_match("^[a-z]+$", "AbCdEf", RX_ICASE, 1);
    ck_match("^[^a-z]+$", "AbCdEf", RX_ICASE, 0);       /* negation + icase */
    ck_match("(x)\\1", "xX", RX_ICASE, 1);              /* icase backref */

    /* Hex and control escapes. */
    ck_match("\\x41\\x42", "AB", 0, 1);
    ck_match("a\\tb", "a\tb", 0, 1);
    ck_match("\\x4", "\x04", 0, 1);                     /* one hex digit */
    ck_match("\\x6a", "j", 0, 1);                       /* lowercase hex */
    ck_match("\\x4A", "J", 0, 1);                       /* uppercase hex */
    ck_match("\\xz", "xz", 0, 1);                       /* lone \x literal */
    ck_match("\\n\\r\\f\\v\\a", "\n\r\f\v\a", 0, 1);    /* control escapes */

    /* Interval edge forms. */
    ck_match("^a{2,}$", "aaa", 0, 1);                   /* unbounded upper */
    ck_match("^a{2,}$", "a", 0, 0);
    ck_match("a{2,x}", "a{2,x}", 0, 1);                 /* malformed, literal */
    ck_match("a{2z}", "a{2z}", 0, 1);                   /* malformed, literal */

    /* Literal metacharacters via escaping. */
    ck_match("a\\.c", "a.c", 0, 1);
    ck_match("a\\.c", "abc", 0, 0);
    ck_match("\\(\\)", "()", 0, 1);
    ck_match("a\\+", "a+", 0, 1);
    ck_match("100\\$", "100$", 0, 1);
    ck_match("a{", "a{", 0, 1);                         /* bare { literal */
    ck_match("a{x}", "a{x}", 0, 1);                     /* invalid interval */

    /* Invalid patterns must be rejected, not crash. */
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

    /* The \0 escape compiles to a NUL byte matcher. */
    ck_match("\\0", "", 0, 0);

    /* Substitution variety. */
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

    /* Parser recursion is bounded (F4): a pattern nested past the depth
     * limit is a clean compile error, not a stack overflow. A deep but
     * legal nest still compiles. */
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
        ck_match(deep, "a", 0, 1);                      /* legal deep nest */
    }
}

/****************************************************************
 * Robustness: long inputs and catastrophic backtracking
 ****************************************************************/

static void
robustness(void)
{
    const char *err;
    size_t big = 200000;
    char *buf = malloc(big + 1);
    rx_t *re;
    rx_match m[2];
    int i;

    /* A long greedy match must not overflow the native stack. */
    memset(buf, 'a', big);
    buf[big] = '\0';
    re = rx_compile(".*", 0, &err);
    tests++;
    if (!re || rx_exec(re, buf, big, 0, m, 1) != 1 ||
        m[0].eo != (long)big) {
        fails++;
        printf("FAIL long greedy .* match\n");
    }
    rx_free(re);

    /* Long backreference. */
    re = rx_compile("(a+)\\1", 0, &err);
    tests++;
    if (!re || rx_exec(re, buf, big, 0, m, 2) != 1) {
        fails++;
        printf("FAIL long backreference\n");
    }
    rx_free(re);

    /* Catastrophic pattern must hit the step budget and return an error
     * rather than hang. */
    {
        char bad[64];

        for (i = 0; i < 40; i++)
            bad[i] = 'a';
        bad[40] = 'X';
        bad[41] = '\0';
        re = rx_compile("(a+)+$", 0, &err);
        tests++;
        if (!re || rx_exec(re, bad, strlen(bad), 0, m, 2) != -1) {
            fails++;
            printf("FAIL catastrophic backtracking not bounded\n");
        }
        rx_free(re);
    }

    /* nmatch of 0 with a NULL match array must be safe. */
    re = rx_compile("abc", 0, &err);
    tests++;
    if (!re || rx_exec(re, "zabc", 4, 0, NULL, 0) != 1) {
        fails++;
        printf("FAIL exec with nmatch 0\n");
    }
    rx_free(re);

    free(buf);
}

/* NULL-safe entry points and the group count accessor. */
static void
api_edge(void)
{
    const char *err;
    rx_t *re = rx_compile("(a)(b)(c)", 0, &err);

    tests++;
    if (!re || rx_ngroups(re) != 3) {
        fails++;
        printf("FAIL rx_ngroups\n");
    }
    rx_free(re);

    tests++;
    if (rx_ngroups(NULL) != 0 ||
        rx_exec(NULL, "x", 1, 0, NULL, 0) != -1 ||
        rx_replace(NULL, "x", 1, "y", 0) != NULL) {
        fails++;
        printf("FAIL NULL-safe entry points\n");
    }
    rx_free(NULL);      /* must be a no-op */
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
    fault_suite();
    fuzz(iters);        /* iters == 0 skips the fuzzer */

    printf("%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
