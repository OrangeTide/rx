/* selftest.c : fast public-API smoke test for the regex engine */
/*
 * Exercises the engine through its public interface only. Build and run
 * via the Makefile `test` target. It includes ../rx.c directly for a
 * one-command build, but since it touches nothing beyond rx.h it could
 * equally link against a separately compiled rx.o.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "../rx.c"

static int tests, fails;

static int
match(const char *pat, const char *s, int flags)
{
    const char *err;
    rx_t *re = rx_compile(pat, flags, &err);
    rx_match m[10];
    int r;

    if (!re) {
        fprintf(stderr, "compile failed for /%s/: %s\n", pat, err);
        return -1;
    }
    r = rx_exec(re, s, strlen(s), 0, m, 10);
    rx_free(re);
    return r;
}

static void
expect_match(const char *pat, const char *s, int flags, int want)
{
    int got = match(pat, s, flags);

    tests++;
    if (got != want) {
        fails++;
        printf("FAIL match /%s/ on \"%s\": got %d want %d\n",
               pat, s, got, want);
    }
}

static void
expect_sub(const char *pat, const char *s, const char *repl, int flags,
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
        printf("FAIL sub /%s/ \"%s\" -> \"%s\": got \"%s\" want \"%s\"\n",
               pat, s, repl, got ? got : "(null)", want);
    }
    free(got);
}

int
main(void)
{
    /* Literals and the dot. */
    expect_match("abc", "xabcy", 0, 1);
    expect_match("a.c", "abc", 0, 1);
    expect_match("a.c", "a\nc", 0, 0);
    expect_match("a.c", "a\nc", RX_DOTALL, 1);

    /* Quantifiers. */
    expect_match("ab*c", "ac", 0, 1);
    expect_match("ab*c", "abbbc", 0, 1);
    expect_match("ab+c", "ac", 0, 0);
    expect_match("ab+c", "abc", 0, 1);
    expect_match("ab?c", "ac", 0, 1);
    expect_match("colou?r", "color", 0, 1);

    /* Intervals. */
    expect_match("a{3}", "aaa", 0, 1);
    expect_match("a{3}", "aa", 0, 0);
    expect_match("a{2,3}", "aaaa", 0, 1);
    expect_match("^a{2,3}$", "aaaa", 0, 0);
    expect_match("^a{2,}$", "aaaaa", 0, 1);
    expect_match("^a{,2}$", "aa", 0, 1);
    expect_match("^a{,2}$", "aaa", 0, 0);

    /* Anchors. */
    expect_match("^abc$", "abc", 0, 1);
    expect_match("^abc$", "xabc", 0, 0);
    expect_match("^b$", "a\nb\nc", RX_MULTILINE, 1);

    /* Classes. */
    expect_match("[abc]+", "cab", 0, 1);
    expect_match("[^abc]", "a", 0, 0);
    expect_match("[a-z]+", "Hello", 0, 1);
    expect_match("[[:digit:]]+", "x123", 0, 1);
    expect_match("[[:alpha:]]+", "123", 0, 0);

    /* Shorthand classes. */
    expect_match("\\d{3}", "ab123", 0, 1);
    expect_match("\\w+", "foo_bar", 0, 1);
    expect_match("\\s", "a b", 0, 1);
    expect_match("\\D", "5", 0, 0);

    /* Word boundaries. */
    expect_match("\\bcat\\b", "a cat here", 0, 1);
    expect_match("\\bcat\\b", "concatenate", 0, 0);
    expect_match("\\<cat\\>", "the cat", 0, 1);

    /* Groups, alternation, backreferences. */
    expect_match("(ab)+", "ababab", 0, 1);
    expect_match("gr(a|e)y", "grey", 0, 1);
    expect_match("gr(a|e)y", "groy", 0, 0);
    expect_match("(.)\\1", "aa", 0, 1);
    expect_match("(.)\\1", "ab", 0, 0);
    expect_match("(?:ab)+c", "ababc", 0, 1);

    /* Case insensitivity. */
    expect_match("hello", "HELLO", RX_ICASE, 1);
    expect_match("[a-f]+", "ABCDEF", RX_ICASE, 1);

    /* Lazy quantifiers via capture inspection. */
    {
        const char *err;
        rx_t *re = rx_compile("<(.*?)>", 0, &err);
        rx_match m[2];

        assert(re);
        assert(rx_exec(re, "<a><b>", 6, 0, m, 2) == 1);
        assert(m[1].eo - m[1].so == 1);        /* lazy: matched "a" only */
        rx_free(re);
        tests++;
    }

    /* Substitution. */
    expect_sub("o", "foo", "0", 0, "f0o");
    expect_sub("o", "foo", "0", RX_GLOBAL, "f00");
    expect_sub("(\\w+)@(\\w+)", "user@host", "\\2.\\1", 0, "host.user");
    expect_sub("a", "banana", "[&]", RX_GLOBAL, "b[a]n[a]n[a]");
    expect_sub("[a-z]+", "hello world", "\\U&", RX_GLOBAL,
               "HELLO WORLD");
    expect_sub("(\\w)(\\w*)", "john", "\\u\\1\\2", 0, "John");
    expect_sub("x*", "abc", "-", RX_GLOBAL, "-a-b-c-");
    expect_sub("a*", "aa", "-", RX_GLOBAL, "-");
    expect_sub("a*", "aba", "-", RX_GLOBAL, "-b-");

    printf("%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
