/* regex.c : compact POSIX-ERE regex engine with convenience extensions */
/*
 * A single-file regular expression matcher and substitution engine meant
 * to be dropped into a sed, a vi clone, a grep, or any small text tool.
 * It is a backtracking engine, so it supports backreferences and the
 * common conveniences found in GNU sed, Vim, and PCRE on top of the
 * POSIX ERE core.
 *
 * Supported pattern syntax (ERE base, backslash makes a metacharacter
 * literal):
 *
 *   literals, .            any char (not newline unless RX_DOTALL)
 *   * + ?                  greedy quantifiers
 *   *? +? ??               lazy (non-greedy) quantifiers
 *   {n} {n,} {n,m} {,m}    counted repetition (and lazy variants)
 *   [...] [^...]           bracket expressions, ranges, [:class:]
 *   ^ $                    anchors (line anchors under RX_MULTILINE)
 *   ( ) (?:...)            capturing and non-capturing groups
 *   |                      alternation
 *   \1 .. \9               backreferences
 *   \d \D \w \W \s \S      Perl-style shorthand classes
 *   \b \B                  word boundary / non-boundary
 *   \< \>                  start-of-word / end-of-word (Vim, GNU)
 *   \n \t \r \f \v \a \0   control escapes, \xHH hex escape
 *
 * Substitution template syntax (rx_replace):
 *
 *   & or \0                whole match
 *   \1 .. \9               captured group
 *   \U \L                  upcase / downcase following output until \E
 *   \u \l                  upcase / downcase next output char
 *   \E                     end case conversion
 *   \n \t ... \xHH         control escapes
 *   \& \\                  literal & and backslash
 *
 * Matching is leftmost, with greedy-by-default preference (Perl/PCRE
 * semantics), not POSIX leftmost-longest.
 *
 * The matcher backtracks using heap-allocated choice and undo stacks
 * rather than the C call stack, so match depth is bounded by available
 * memory, not the native stack. A per-search step budget guards against
 * catastrophic backtracking by failing the search rather than hanging.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/****************************************************************
 * Public interface
 *
 * This lives inline so the engine is a single file. To embed it,
 * copy this block into a regex.h and include it where needed.
 ****************************************************************/

/* Compile and match flags. The match flags (bits 0..2) are stored in the
 * compiled object; RX_GLOBAL only affects rx_replace. */
#define RX_ICASE     0x01   /* case-insensitive matching          */
#define RX_MULTILINE 0x02   /* ^ and $ match at embedded newlines  */
#define RX_DOTALL    0x04   /* . also matches newline              */
#define RX_GLOBAL    0x08   /* rx_replace: replace every match     */

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

/****************************************************************/

#define OK   0
#define ERR  (-1)

/* Per-search instruction budget. A search that exceeds it fails with an
 * error rather than looping on a pathological pattern. */
#ifndef RX_STEP_LIMIT
#define RX_STEP_LIMIT 20000000L
#endif

/* Largest repetition count accepted in a {n,m} interval, matching the
 * POSIX RE_DUP_MAX minimum. Keeps a huge count from blowing up the
 * compiled program. */
#ifndef RX_DUP_MAX
#define RX_DUP_MAX 32767
#endif

/* Largest group-nesting depth the recursive-descent parser accepts. The
 * parser recurses on the C stack for each nested group, so this bounds
 * that recursion to a compile error rather than a stack overflow. */
#ifndef RX_MAX_DEPTH
#define RX_MAX_DEPTH 1000
#endif

/****************************************************************
 * Instruction set for the backtracking virtual machine
 ****************************************************************/

enum {
    I_CHAR,     /* match one literal character (c)            */
    I_ANY,      /* match any character (newline per flags)    */
    I_CLASS,    /* match a character in the 256-bit set       */
    I_BOL,      /* assert beginning of text or line           */
    I_EOL,      /* assert end of text or line                 */
    I_WB,       /* assert word boundary                       */
    I_NWB,      /* assert not a word boundary                 */
    I_WSTART,   /* assert start of word (\<)                  */
    I_WEND,     /* assert end of word (\>)                    */
    I_SAVE,     /* record current position in slot x          */
    I_BREF,     /* match backreference to group x             */
    I_JMP,      /* jump to x                                  */
    I_SPLIT,    /* try x first, then y on failure             */
    I_MARK,     /* record position in scratch slot x (no undo) */
    I_PROGRESS, /* if position == scratch slot x, jump to y    */
    I_MATCH,    /* accept                                     */
};

typedef struct {
    int op;
    int x, y;           /* jump targets, save slot, or group number */
    unsigned char c;    /* literal for I_CHAR                       */
    unsigned char *set; /* 32-byte bitmap for I_CLASS (not owned)   */
} rx_inst;

struct rx {
    rx_inst *prog;
    int plen;
    int ngroup;             /* number of capturing groups (0 == none) */
    int nmark;              /* scratch slots for repetition guards    */
    int flags;
    unsigned char **sets;   /* owned class bitmaps, freed in rx_free  */
    int nsets;
};

/****************************************************************
 * Bitmap helpers for character classes
 ****************************************************************/

static void
set_bit(unsigned char *set, int c)
{
    set[(c & 0xff) >> 3] |= (unsigned char)(1u << (c & 7));
}

static int
get_bit(const unsigned char *set, int c)
{
    return (set[(c & 0xff) >> 3] >> (c & 7)) & 1;
}

static void
set_range(unsigned char *set, int lo, int hi)
{
    int i;

    for (i = lo; i <= hi; i++)
        set_bit(set, i);
}

static void
invert_set(unsigned char *set)
{
    int i;

    for (i = 0; i < 32; i++)
        set[i] = (unsigned char)~set[i];
}

static int
is_word(int c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/****************************************************************
 * The backtracking matcher
 ****************************************************************/

/* A pending alternative: resume at `pc`/`sp`, after undoing every capture
 * recorded since this point (down to undo-log length `ulen`). */
typedef struct {
    int pc;
    long sp;
    long ulen;
} rx_choice;

/* One capture slot change, so backtracking can roll it back. */
typedef struct {
    int slot;
    long old;
} rx_undo;

typedef struct {
    rx_inst *prog;
    const char *text;
    long len;
    int flags;
    long *sav;          /* capture slots, 2 per group plus group 0 */
    int nsav;
    long steps;         /* remaining instruction budget            */
    int aborted;        /* set on budget exhaustion or allocation failure */

    rx_choice *cs;      /* choice-point (backtracking) stack       */
    long cn, ccap;
    rx_undo *ul;        /* capture undo log                        */
    long un, ucap;
} rx_ctx;

static int
chr_eq(rx_ctx *c, int a, int b)
{
    if (c->flags & RX_ICASE)
        return tolower((unsigned char)a) == tolower((unsigned char)b);
    return a == b;
}

/* Class membership. Case-insensitive bracket classes are folded at
 * compile time, so the flags are not consulted here. */
static int
in_class(rx_ctx *c, const unsigned char *set, int ch)
{
    (void)c;
    return get_bit(set, ch);
}

/* Record a capture-slot change on the undo log. Returns ERR on
 * allocation failure. */
static int
log_save(rx_ctx *c, int slot, long old)
{
    if (c->un >= c->ucap) {
        long ncap = c->ucap ? c->ucap * 2 : 64;
        rx_undo *nu = realloc(c->ul, (size_t)ncap * sizeof *nu);

        if (!nu)
            return ERR;
        c->ul = nu;
        c->ucap = ncap;
    }
    c->ul[c->un].slot = slot;
    c->ul[c->un].old = old;
    c->un++;
    return OK;
}

/* Push a pending alternative. Returns ERR on allocation failure. */
static int
push_choice(rx_ctx *c, int pc, long sp)
{
    if (c->cn >= c->ccap) {
        long ncap = c->ccap ? c->ccap * 2 : 64;
        rx_choice *nc = realloc(c->cs, (size_t)ncap * sizeof *nc);

        if (!nc)
            return ERR;
        c->cs = nc;
        c->ccap = ncap;
    }
    c->cs[c->cn].pc = pc;
    c->cs[c->cn].sp = sp;
    c->cs[c->cn].ulen = c->un;
    c->cn++;
    return OK;
}

/* Iterative backtracking matcher. Choice points and capture undo live on
 * heap stacks rather than the C call stack, so match depth is bounded by
 * available memory, not by the native stack. Returns the text offset
 * where the match completes, or -1 on failure (c->aborted distinguishes
 * a hard error such as the step budget or an allocation failure). */
static long
rx_run(rx_ctx *c, int pc, long sp)
{
    c->cn = 0;
    c->un = 0;

    for (;;) {
        rx_inst *in;
        int fail = 0;

        if (--c->steps < 0) {
            c->aborted = 1;
            return -1;
        }
        in = &c->prog[pc];

        switch (in->op) {
        case I_CHAR:
            if (sp < c->len && chr_eq(c, c->text[sp], in->c)) {
                pc++;
                sp++;
            } else {
                fail = 1;
            }
            break;

        case I_ANY:
            if (sp < c->len &&
                ((c->flags & RX_DOTALL) || c->text[sp] != '\n')) {
                pc++;
                sp++;
            } else {
                fail = 1;
            }
            break;

        case I_CLASS:
            if (sp < c->len &&
                in_class(c, in->set, (unsigned char)c->text[sp])) {
                pc++;
                sp++;
            } else {
                fail = 1;
            }
            break;

        case I_BOL:
            if (sp == 0 ||
                ((c->flags & RX_MULTILINE) && c->text[sp - 1] == '\n'))
                pc++;
            else
                fail = 1;
            break;

        case I_EOL:
            if (sp == c->len ||
                ((c->flags & RX_MULTILINE) && c->text[sp] == '\n'))
                pc++;
            else
                fail = 1;
            break;

        case I_WB:
        case I_NWB:
        case I_WSTART:
        case I_WEND: {
            int left = sp > 0 ? is_word((unsigned char)c->text[sp - 1]) : 0;
            int right = sp < c->len ? is_word((unsigned char)c->text[sp]) : 0;
            int ok;

            if (in->op == I_WB)
                ok = left != right;
            else if (in->op == I_NWB)
                ok = left == right;
            else if (in->op == I_WSTART)
                ok = right && !left;
            else
                ok = left && !right;
            if (ok)
                pc++;
            else
                fail = 1;
            break;
        }

        case I_SAVE:
            if (in->x < c->nsav) {
                if (log_save(c, in->x, c->sav[in->x]) != OK) {
                    c->aborted = 1;
                    return -1;
                }
                c->sav[in->x] = sp;
            }
            pc++;
            break;

        case I_BREF: {
            int g = in->x;
            long s, e, n, i;

            if (2 * g + 1 >= c->nsav) {
                pc++;
                break;
            }
            s = c->sav[2 * g];
            e = c->sav[2 * g + 1];
            if (s < 0 || e < 0) {       /* unset group matches empty */
                pc++;
                break;
            }
            n = e - s;
            if (sp + n > c->len) {
                fail = 1;
                break;
            }
            for (i = 0; i < n; i++) {
                if (!chr_eq(c, c->text[sp + i], c->text[s + i])) {
                    fail = 1;
                    break;
                }
            }
            if (!fail) {
                sp += n;
                pc++;
            }
            break;
        }

        case I_JMP:
            pc = in->x;
            break;

        case I_SPLIT:
            if (push_choice(c, in->y, sp) != OK) {
                c->aborted = 1;
                return -1;
            }
            pc = in->x;
            break;

        case I_MARK:
            /* Record the iteration-entry position. No undo entry: the
             * slot is always re-marked before the matching I_PROGRESS
             * reads it, so it never needs restoring on backtrack. */
            if (in->x < c->nsav)
                c->sav[in->x] = sp;
            pc++;
            break;

        case I_PROGRESS:
            /* A repetition body that consumed nothing: take the loop
             * exit instead of looping forever. */
            if (in->x < c->nsav && c->sav[in->x] == sp)
                pc = in->y;
            else
                pc++;
            break;

        case I_MATCH:
            return sp;

        default:
            fail = 1;
            break;
        }

        if (fail) {
            rx_choice cp;

            if (c->cn == 0)
                return -1;
            cp = c->cs[--c->cn];
            while (c->un > cp.ulen) {
                c->un--;
                c->sav[c->ul[c->un].slot] = c->ul[c->un].old;
            }
            pc = cp.pc;
            sp = cp.sp;
        }
    }
}

/****************************************************************
 * Compiler state and low-level emit helpers
 ****************************************************************/

typedef struct {
    const char *p;          /* current parse position */
    const char *pend;
    rx_inst *prog;
    int plen, pcap;
    int ngroup;
    int nmark;              /* scratch slots allocated for repetition guards */
    int maxref;             /* highest backreference seen */
    int depth;              /* current group-nesting depth */
    int flags;
    const char *err;
    unsigned char **sets;
    int nsets, setcap;
} comp;

/* A detached copy of a run of instructions, with jump targets stored
 * relative to the start of the run so the run can be re-emitted at any
 * offset. Used to expand quantifiers and lay out alternation branches. */
typedef struct {
    rx_inst *in;
    int n;
} tpl;

static int
emit(comp *c, int op)
{
    rx_inst *in;

    if (c->err)
        return 0;
    if (c->plen >= c->pcap) {
        int ncap = c->pcap ? c->pcap * 2 : 64;
        rx_inst *np = realloc(c->prog, (size_t)ncap * sizeof *np);

        if (!np) {
            c->err = "out of memory";
            return 0;
        }
        c->prog = np;
        c->pcap = ncap;
    }
    in = &c->prog[c->plen];
    memset(in, 0, sizeof *in);
    in->op = op;
    return c->plen++;
}

static unsigned char *
new_set(comp *c)
{
    unsigned char *set;

    if (c->err)
        return NULL;
    if (c->nsets >= c->setcap) {
        int ncap = c->setcap ? c->setcap * 2 : 8;
        unsigned char **ns = realloc(c->sets, (size_t)ncap * sizeof *ns);

        if (!ns) {
            c->err = "out of memory";
            return NULL;
        }
        c->sets = ns;
        c->setcap = ncap;
    }
    set = calloc(32, 1);
    if (!set) {
        c->err = "out of memory";
        return NULL;
    }
    c->sets[c->nsets++] = set;
    return set;
}

/* Detach prog[from .. plen) into a template and truncate the program
 * back to `from`. Jump targets inside the run are rebased to zero. */
static tpl
take_tpl(comp *c, int from)
{
    tpl t;
    int i;

    t.n = c->plen - from;
    t.in = NULL;
    if (c->err || t.n <= 0) {
        t.n = t.n < 0 ? 0 : t.n;
        c->plen = from;
        return t;
    }
    t.in = malloc((size_t)t.n * sizeof *t.in);
    if (!t.in) {
        c->err = "out of memory";
        t.n = 0;
        c->plen = from;
        return t;
    }
    for (i = 0; i < t.n; i++) {
        t.in[i] = c->prog[from + i];
        if (t.in[i].op == I_JMP) {
            t.in[i].x -= from;
        } else if (t.in[i].op == I_SPLIT) {
            t.in[i].x -= from;
            t.in[i].y -= from;
        } else if (t.in[i].op == I_PROGRESS) {
            t.in[i].y -= from;      /* x is a scratch slot, not a target */
        }
    }
    c->plen = from;
    return t;
}

/* Append a template at the current program end, rebasing jump targets. */
static void
put_tpl(comp *c, tpl t)
{
    int base, i;

    if (c->err)
        return;
    base = c->plen;
    for (i = 0; i < t.n; i++) {
        int at = emit(c, t.in[i].op);
        rx_inst *in;

        if (c->err)
            return;
        in = &c->prog[at];
        *in = t.in[i];
        if (in->op == I_JMP) {
            in->x += base;
        } else if (in->op == I_SPLIT) {
            in->x += base;
            in->y += base;
        } else if (in->op == I_PROGRESS) {
            in->y += base;          /* x is a scratch slot, not a target */
        }
    }
}

/****************************************************************
 * Recursive-descent compiler
 ****************************************************************/

static void parse_alt(comp *c);
static void parse_concat(comp *c);
static void parse_repeat(comp *c);
static void parse_atom(comp *c);

static int
hexval(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

/* Read a backslash escape that resolves to a single literal byte. The
 * backslash has already been consumed. Returns the byte value. */
static int
read_escape_char(comp *c)
{
    int e;

    if (c->p >= c->pend) {
        c->err = "trailing backslash";
        return 0;
    }
    e = (unsigned char)*c->p++;
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return '\a';
    case '0': return '\0';
    case 'x': {
        int h1, h2, v;

        if (c->p >= c->pend || (h1 = hexval((unsigned char)*c->p)) < 0)
            return 'x';         /* lone \x is a literal x */
        c->p++;
        v = h1;
        if (c->p < c->pend && (h2 = hexval((unsigned char)*c->p)) >= 0) {
            c->p++;
            v = v * 16 + h2;
        }
        return v;
    }
    default:
        return e;               /* literal: covers \\ \. \* \( and so on */
    }
}

/* Fill `set` (expected zeroed) with the members of a Perl-style shorthand
 * letter, inverting for the upper-case (negated) forms. */
static void
fill_shorthand(unsigned char *set, int letter)
{
    int negate = 0;

    switch (letter) {
    case 'D': negate = 1; /* fall through */
    case 'd':
        set_range(set, '0', '9');
        break;
    case 'W': negate = 1; /* fall through */
    case 'w':
        set_range(set, 'a', 'z');
        set_range(set, 'A', 'Z');
        set_range(set, '0', '9');
        set_bit(set, '_');
        break;
    case 'S': negate = 1; /* fall through */
    case 's':
        set_bit(set, ' ');
        set_bit(set, '\t');
        set_bit(set, '\n');
        set_bit(set, '\r');
        set_bit(set, '\f');
        set_bit(set, '\v');
        break;
    }
    if (negate)
        invert_set(set);
}

/* Fill a fresh class set for a Perl-style shorthand letter. */
static unsigned char *
shorthand_set(comp *c, int letter)
{
    unsigned char *set = new_set(c);

    if (!set)
        return NULL;
    fill_shorthand(set, letter);
    return set;
}

/* Add a POSIX [:name:] class to a bracket-expression set. On entry p
 * points just past "[:" . Returns OK, or ERR with c->err set. */
static int
add_posix_class(comp *c, unsigned char *set)
{
    const char *start = c->p;
    char name[16];
    size_t n = 0;
    int i;

    while (c->p < c->pend && *c->p != ':') {
        if (n < sizeof name - 1)
            name[n++] = *c->p;
        c->p++;
    }
    name[n] = '\0';
    if (c->p + 1 >= c->pend || c->p[0] != ':' || c->p[1] != ']') {
        c->p = start;
        c->err = "bad [: :] class";
        return ERR;
    }
    c->p += 2;      /* consume ":]" */

    for (i = 0; i < 256; i++) {
        int hit = 0;

        if (!strcmp(name, "alpha")) hit = isalpha(i);
        else if (!strcmp(name, "digit")) hit = isdigit(i);
        else if (!strcmp(name, "alnum")) hit = isalnum(i);
        else if (!strcmp(name, "space")) hit = isspace(i);
        else if (!strcmp(name, "upper")) hit = isupper(i);
        else if (!strcmp(name, "lower")) hit = islower(i);
        else if (!strcmp(name, "punct")) hit = ispunct(i);
        else if (!strcmp(name, "xdigit")) hit = isxdigit(i);
        else if (!strcmp(name, "cntrl")) hit = iscntrl(i);
        else if (!strcmp(name, "print")) hit = isprint(i);
        else if (!strcmp(name, "graph")) hit = isgraph(i);
        else if (!strcmp(name, "blank")) hit = (i == ' ' || i == '\t');
        else {
            c->err = "unknown [: :] class";
            return ERR;
        }
        if (hit)
            set_bit(set, i);
    }
    return OK;
}

/* Read one character inside a bracket expression, resolving escapes. */
static int
bracket_char(comp *c)
{
    if (*c->p == '\\') {
        c->p++;
        return read_escape_char(c);
    }
    return (unsigned char)*c->p++;
}

static void
parse_class(comp *c)
{
    unsigned char *set = new_set(c);
    int negate = 0;
    int first = 1;

    if (c->err)
        return;
    c->p++;             /* consume '[' */
    if (c->p < c->pend && *c->p == '^') {
        negate = 1;
        c->p++;
    }
    while (c->p < c->pend) {
        int lo;

        if (*c->p == ']' && !first)
            break;
        first = 0;

        if (*c->p == '[' && c->p + 1 < c->pend && c->p[1] == ':') {
            c->p += 2;
            if (add_posix_class(c, set) != OK)
                return;
            continue;
        }

        /* A shorthand class (\d \D \w \W \s \S) contributes its whole
         * membership to the bracket set rather than a single byte. */
        if (*c->p == '\\' && c->p + 1 < c->pend &&
            strchr("dDwWsS", (unsigned char)c->p[1])) {
            unsigned char tmp[32];
            int i;

            memset(tmp, 0, sizeof tmp);
            fill_shorthand(tmp, (unsigned char)c->p[1]);
            for (i = 0; i < 32; i++)
                set[i] |= tmp[i];
            c->p += 2;
            continue;
        }

        lo = bracket_char(c);
        if (c->err)
            return;
        if (c->p + 1 <= c->pend && *c->p == '-' &&
            c->p + 1 < c->pend && c->p[1] != ']') {
            int hi;

            c->p++;             /* consume '-' */
            hi = bracket_char(c);
            if (c->err)
                return;
            if (hi < lo) {
                c->err = "bad range in [ ]";
                return;
            }
            set_range(set, lo, hi);
        } else {
            set_bit(set, lo);
        }
    }
    if (c->p >= c->pend || *c->p != ']') {
        c->err = "unterminated [ ]";
        return;
    }
    c->p++;             /* consume ']' */

    /* Case folding must happen before negation so that a negated class
     * excludes both cases of each letter. */
    if (c->flags & RX_ICASE) {
        int i;

        for (i = 0; i < 256; i++) {
            if (get_bit(set, i) && isalpha(i)) {
                set_bit(set, tolower(i));
                set_bit(set, toupper(i));
            }
        }
    }
    if (negate)
        invert_set(set);

    {
        int at = emit(c, I_CLASS);

        if (!c->err)
            c->prog[at].set = set;
    }
}

static void
parse_escape(comp *c)
{
    int e;

    if (c->p >= c->pend) {
        c->err = "trailing backslash";
        return;
    }
    e = (unsigned char)*c->p;
    switch (e) {
    case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
        unsigned char *set;

        c->p++;
        set = shorthand_set(c, e);
        if (set) {
            int at = emit(c, I_CLASS);

            if (!c->err)
                c->prog[at].set = set;
        }
        return;
    }
    case 'b': c->p++; emit(c, I_WB); return;
    case 'B': c->p++; emit(c, I_NWB); return;
    case '<': c->p++; emit(c, I_WSTART); return;
    case '>': c->p++; emit(c, I_WEND); return;
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9': {
        int g = e - '0';
        int at;

        c->p++;
        if (g > c->maxref)
            c->maxref = g;
        at = emit(c, I_BREF);
        if (!c->err)
            c->prog[at].x = g;
        return;
    }
    default: {
        int ch = read_escape_char(c);
        int at;

        at = emit(c, I_CHAR);
        if (!c->err)
            c->prog[at].c = (unsigned char)ch;
        return;
    }
    }
}

static void
parse_atom(comp *c)
{
    int ch;

    if (c->err)
        return;
    if (c->p >= c->pend) {
        c->err = "unexpected end of pattern";
        return;
    }
    ch = (unsigned char)*c->p;
    switch (ch) {
    case '(': {
        int capturing = 1;
        int g = 0;

        c->p++;
        if (c->p + 1 < c->pend && c->p[0] == '?' && c->p[1] == ':') {
            c->p += 2;
            capturing = 0;
        }
        if (++c->depth > RX_MAX_DEPTH) {
            c->err = "pattern nested too deeply";
            return;
        }
        if (capturing) {
            g = ++c->ngroup;
            {
                int at = emit(c, I_SAVE);
                if (!c->err)
                    c->prog[at].x = 2 * g;
            }
        }
        parse_alt(c);
        c->depth--;
        if (c->err)
            return;
        if (c->p >= c->pend || *c->p != ')') {
            c->err = "missing )";
            return;
        }
        c->p++;
        if (capturing) {
            int at = emit(c, I_SAVE);
            if (!c->err)
                c->prog[at].x = 2 * g + 1;
        }
        return;
    }
    case '[':
        parse_class(c);
        return;
    case '.':
        c->p++;
        emit(c, I_ANY);
        return;
    case '^':
        c->p++;
        emit(c, I_BOL);
        return;
    case '$':
        c->p++;
        emit(c, I_EOL);
        return;
    case '\\':
        c->p++;
        parse_escape(c);
        return;
    case ')':
    case '|':
    case '*':
    case '+':
    case '?':
        c->err = "unexpected quantifier or operator";
        return;
    default: {
        /* Ordinary character. '{' and '}' fall here too and are treated
         * as literals when they are not a valid interval. */
        int at = emit(c, I_CHAR);
        if (!c->err)
            c->prog[at].c = (unsigned char)ch;
        c->p++;
        return;
    }
    }
}

/* Lay out `t` repeated according to {n,m}. m < 0 means unbounded. */
static void
build_rep(comp *c, tpl t, int n, int m, int lazy)
{
    int i;

    for (i = 0; i < n; i++)
        put_tpl(c, t);
    if (c->err)
        return;

    if (m < 0) {
        /* Trailing star appended after the n mandatory copies:
         *
         *   L1: SPLIT body, end      (greedy; swapped when lazy)
         *   body: MARK slot          record the iteration-entry position
         *         <t>
         *         PROGRESS slot, end  exit if the body consumed nothing
         *         JMP L1
         *   end:
         *
         * The MARK/PROGRESS pair stops a nullable body (such as a*) from
         * looping forever with no progress. */
        int slot = c->nmark++;
        int sp = emit(c, I_SPLIT);
        int b, mk, pr, j, end;

        if (c->err)
            return;
        b = c->plen;
        mk = emit(c, I_MARK);
        if (c->err)
            return;
        c->prog[mk].x = slot;       /* rebased to a real slot after parse */
        put_tpl(c, t);
        pr = emit(c, I_PROGRESS);
        j = emit(c, I_JMP);
        if (c->err)
            return;
        c->prog[j].x = sp;
        end = c->plen;
        c->prog[pr].x = slot;
        c->prog[pr].y = end;
        if (lazy) {
            c->prog[sp].x = end;
            c->prog[sp].y = b;
        } else {
            c->prog[sp].x = b;
            c->prog[sp].y = end;
        }
        return;
    }

    {
        int opt = m - n;
        int *splits;
        int k, end;

        if (opt <= 0)
            return;
        splits = malloc((size_t)opt * sizeof *splits);
        if (!splits) {
            c->err = "out of memory";
            return;
        }
        for (k = 0; k < opt; k++) {
            int sp = emit(c, I_SPLIT);
            int b;

            if (c->err) {
                free(splits);
                return;
            }
            b = c->plen;
            put_tpl(c, t);
            if (c->err) {
                free(splits);
                return;
            }
            if (lazy)
                c->prog[sp].y = b;
            else
                c->prog[sp].x = b;
            splits[k] = sp;
        }
        end = c->plen;
        for (k = 0; k < opt; k++) {
            if (lazy)
                c->prog[splits[k]].x = end;
            else
                c->prog[splits[k]].y = end;
        }
        free(splits);
    }
}

/* Parse the quantifier (if any) following an atom whose code occupies
 * prog[from .. plen). */
static void
parse_quantifier(comp *c, int from)
{
    int n = 1, m = 1;
    int have = 0;
    int lazy = 0;
    tpl t;

    if (c->err || c->p >= c->pend)
        return;

    switch (*c->p) {
    case '*':
        n = 0;
        m = -1;
        have = 1;
        c->p++;
        break;
    case '+':
        n = 1;
        m = -1;
        have = 1;
        c->p++;
        break;
    case '?':
        n = 0;
        m = 1;
        have = 1;
        c->p++;
        break;
    case '{': {
        const char *save = c->p;
        const char *q = c->p + 1;
        int lo = 0, hi, sawlo = 0;

        while (q < c->pend && isdigit((unsigned char)*q)) {
            /* Saturate rather than overflow; the cap check below rejects. */
            lo = lo > RX_DUP_MAX ? RX_DUP_MAX + 1 : lo * 10 + (*q - '0');
            sawlo = 1;
            q++;
        }
        if (q < c->pend && *q == ',') {
            q++;
            if (q < c->pend && isdigit((unsigned char)*q)) {
                hi = 0;
                while (q < c->pend && isdigit((unsigned char)*q)) {
                    hi = hi > RX_DUP_MAX ? RX_DUP_MAX + 1 : hi * 10 + (*q - '0');
                    q++;
                }
            } else {
                hi = -1;        /* {n,} unbounded */
            }
        } else {
            if (!sawlo) {
                /* "{" not followed by a count: treat as literal */
                return;
            }
            hi = lo;
        }
        if (q >= c->pend || *q != '}') {
            /* not a valid interval: leave '{' as a literal */
            c->p = save;
            return;
        }
        q++;                    /* consume '}' */
        if (lo > RX_DUP_MAX || hi > RX_DUP_MAX) {
            c->err = "repetition count too large";
            return;
        }
        if (hi >= 0 && hi < lo) {
            c->err = "bad {n,m} interval";
            return;
        }
        n = lo;
        m = hi;
        have = 1;
        c->p = q;
        break;
    }
    default:
        return;
    }

    if (!have)
        return;
    if (c->p < c->pend && *c->p == '?') {
        lazy = 1;
        c->p++;
    }

    t = take_tpl(c, from);
    if (c->err) {
        free(t.in);
        return;
    }
    build_rep(c, t, n, m < 0 ? -1 : m, lazy);
    free(t.in);
}

static void
parse_repeat(comp *c)
{
    int from = c->plen;

    parse_atom(c);
    if (c->err)
        return;
    parse_quantifier(c, from);
}

static void
parse_concat(comp *c)
{
    while (!c->err && c->p < c->pend && *c->p != '|' && *c->p != ')')
        parse_repeat(c);
}

static void
parse_alt(comp *c)
{
    int nbr = 0, cap = 0;
    tpl *branches = NULL;
    int from, i, k;
    int *jmps;

    /* Collect each alternative as a template. */
    for (;;) {
        from = c->plen;
        parse_concat(c);
        if (c->err)
            goto done;
        if (nbr >= cap) {
            int ncap = cap ? cap * 2 : 4;
            tpl *nb = realloc(branches, (size_t)ncap * sizeof *nb);

            if (!nb) {
                c->err = "out of memory";
                goto done;
            }
            branches = nb;
            cap = ncap;
        }
        branches[nbr++] = take_tpl(c, from);
        if (c->err)
            goto done;
        if (c->p < c->pend && *c->p == '|') {
            c->p++;
            continue;
        }
        break;
    }

    if (nbr == 1) {
        put_tpl(c, branches[0]);
        goto done;
    }

    /* Emit nested SPLIT/JMP so earlier branches are preferred. */
    jmps = malloc((size_t)(nbr - 1) * sizeof *jmps);
    if (!jmps) {
        c->err = "out of memory";
        goto done;
    }
    for (i = 0; i < nbr - 1; i++) {
        int sp = emit(c, I_SPLIT);

        if (c->err)
            break;
        c->prog[sp].x = c->plen;
        put_tpl(c, branches[i]);
        jmps[i] = emit(c, I_JMP);
        if (c->err)
            break;
        c->prog[sp].y = c->plen;
    }
    if (!c->err)
        put_tpl(c, branches[nbr - 1]);
    if (!c->err) {
        int end = c->plen;

        for (k = 0; k < nbr - 1; k++)
            c->prog[jmps[k]].x = end;
    }
    free(jmps);

done:
    for (i = 0; i < nbr; i++)
        free(branches[i].in);
    free(branches);
}

/****************************************************************
 * Public API: compile, exec, free
 ****************************************************************/

rx_t *
rx_compile(const char *pattern, int flags, const char **errp)
{
    comp c;
    rx_t *re;
    int at;

    memset(&c, 0, sizeof c);
    c.p = pattern;
    c.pend = pattern + strlen(pattern);
    c.flags = flags;

    emit(&c, I_SAVE);           /* slot 0: whole-match start */
    parse_alt(&c);
    if (!c.err && c.p != c.pend)
        c.err = "unbalanced ) or trailing characters";
    at = emit(&c, I_SAVE);      /* slot 1: whole-match end */
    if (!c.err)
        c.prog[at].x = 1;
    emit(&c, I_MATCH);

    if (!c.err && c.maxref > c.ngroup)
        c.err = "backreference to undefined group";

    /* Repetition-guard scratch slots are numbered from zero during the
     * parse; place them above the capture slots now that the group count
     * is final. */
    if (!c.err && c.nmark > 0) {
        int base = 2 * (c.ngroup + 1);
        int i;

        for (i = 0; i < c.plen; i++) {
            if (c.prog[i].op == I_MARK || c.prog[i].op == I_PROGRESS)
                c.prog[i].x += base;
        }
    }

    if (c.err) {
        int i;

        if (errp)
            *errp = c.err;
        for (i = 0; i < c.nsets; i++)
            free(c.sets[i]);
        free(c.sets);
        free(c.prog);
        return NULL;
    }

    re = malloc(sizeof *re);
    if (!re) {
        int i;

        if (errp)
            *errp = "out of memory";
        for (i = 0; i < c.nsets; i++)
            free(c.sets[i]);
        free(c.sets);
        free(c.prog);
        return NULL;
    }
    re->prog = c.prog;
    re->plen = c.plen;
    re->ngroup = c.ngroup;
    re->nmark = c.nmark;
    re->flags = c.flags;
    re->sets = c.sets;
    re->nsets = c.nsets;
    if (errp)
        *errp = NULL;
    return re;
}

void
rx_free(rx_t *re)
{
    int i;

    if (!re)
        return;
    for (i = 0; i < re->nsets; i++)
        free(re->sets[i]);
    free(re->sets);
    free(re->prog);
    free(re);
}

int
rx_ngroups(const rx_t *re)
{
    return re ? re->ngroup : 0;
}

int
rx_exec(rx_t *re, const char *text, size_t len, size_t start,
        rx_match *m, int nmatch)
{
    rx_ctx ctx;
    long *sav;
    int nsav, i;
    long begin;

    if (!re)
        return ERR;
    nsav = 2 * (re->ngroup + 1) + re->nmark;    /* captures + scratch slots */
    sav = malloc((size_t)nsav * sizeof *sav);
    if (!sav)
        return ERR;

    ctx.prog = re->prog;
    ctx.text = text;
    ctx.len = (long)len;
    ctx.flags = re->flags;
    ctx.sav = sav;
    ctx.nsav = nsav;
    ctx.cs = NULL;
    ctx.cn = ctx.ccap = 0;
    ctx.ul = NULL;
    ctx.un = ctx.ucap = 0;

    for (begin = (long)start; begin <= (long)len; begin++) {
        long r;

        for (i = 0; i < nsav; i++)
            sav[i] = -1;
        ctx.steps = RX_STEP_LIMIT;
        ctx.aborted = 0;

        r = rx_run(&ctx, 0, begin);
        if (ctx.aborted) {
            free(ctx.cs);
            free(ctx.ul);
            free(sav);
            return ERR;
        }
        if (r >= 0) {
            int ng = re->ngroup + 1;

            for (i = 0; i < nmatch; i++) {
                if (i < ng) {
                    m[i].so = sav[2 * i];
                    m[i].eo = sav[2 * i + 1];
                } else {
                    m[i].so = m[i].eo = -1;
                }
            }
            free(ctx.cs);
            free(ctx.ul);
            free(sav);
            return 1;
        }
    }
    free(ctx.cs);
    free(ctx.ul);
    free(sav);
    return 0;
}

/****************************************************************
 * Substitution
 ****************************************************************/

typedef struct {
    char *b;
    size_t len, cap;
    int oom;
} sbuf;

static void
sb_reserve(sbuf *s, size_t extra)
{
    if (s->oom)
        return;
    if (s->len + extra + 1 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 64;
        char *nb;

        while (ncap < s->len + extra + 1)
            ncap *= 2;
        nb = realloc(s->b, ncap);
        if (!nb) {
            s->oom = 1;
            return;
        }
        s->b = nb;
        s->cap = ncap;
    }
}

static void
sb_putc(sbuf *s, int ch)
{
    sb_reserve(s, 1);
    if (s->oom)
        return;
    s->b[s->len++] = (char)ch;
}

static void
sb_put(sbuf *s, const char *p, size_t n)
{
    sb_reserve(s, n);
    if (s->oom)
        return;
    memcpy(s->b + s->len, p, n);
    s->len += n;
}

/* Case-conversion state for the replacement template. */
typedef struct {
    int mode;       /* 0 none, 'U' upper, 'L' lower (sticky)   */
    int once;       /* 0 none, 'u' upper, 'l' lower (one char) */
} cstate;

static void
put_cased(sbuf *out, cstate *cs, int ch)
{
    if (cs->once == 'u') {
        ch = toupper((unsigned char)ch);
        cs->once = 0;
    } else if (cs->once == 'l') {
        ch = tolower((unsigned char)ch);
        cs->once = 0;
    } else if (cs->mode == 'U') {
        ch = toupper((unsigned char)ch);
    } else if (cs->mode == 'L') {
        ch = tolower((unsigned char)ch);
    }
    sb_putc(out, ch);
}

static void
put_group(sbuf *out, cstate *cs, const char *text, rx_match *m,
          int ng, int g)
{
    long i;

    if (g < 0 || g >= ng)
        return;
    if (m[g].so < 0 || m[g].eo < 0)
        return;
    for (i = m[g].so; i < m[g].eo; i++)
        put_cased(out, cs, (unsigned char)text[i]);
}

/* Expand one replacement template against the current match. */
static void
expand(sbuf *out, const char *repl, const char *text, rx_match *m, int ng)
{
    const char *r = repl;
    cstate cs = { 0, 0 };

    while (*r) {
        if (*r == '&') {
            put_group(out, &cs, text, m, ng, 0);
            r++;
            continue;
        }
        if (*r != '\\') {
            put_cased(out, &cs, (unsigned char)*r);
            r++;
            continue;
        }
        r++;                    /* consume backslash */
        if (!*r) {
            sb_putc(out, '\\');
            break;
        }
        switch (*r) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            put_group(out, &cs, text, m, ng, *r - '0');
            break;
        case '&': put_cased(out, &cs, '&'); break;
        case '\\': put_cased(out, &cs, '\\'); break;
        case 'n': sb_putc(out, '\n'); break;
        case 't': sb_putc(out, '\t'); break;
        case 'r': sb_putc(out, '\r'); break;
        case 'f': sb_putc(out, '\f'); break;
        case 'v': sb_putc(out, '\v'); break;
        case 'a': sb_putc(out, '\a'); break;
        case 'x': {
            int h1, h2, v;

            if ((h1 = hexval((unsigned char)r[1])) < 0) {
                put_cased(out, &cs, 'x');   /* lone \x is literal x */
                break;
            }
            r++;                            /* consume first hex digit */
            v = h1;
            if ((h2 = hexval((unsigned char)r[1])) >= 0) {
                r++;                        /* consume second hex digit */
                v = v * 16 + h2;
            }
            sb_putc(out, v);
            break;
        }
        case 'U': cs.mode = 'U'; break;
        case 'L': cs.mode = 'L'; break;
        case 'E': cs.mode = 0; cs.once = 0; break;
        case 'u': cs.once = 'u'; break;
        case 'l': cs.once = 'l'; break;
        default:
            put_cased(out, &cs, (unsigned char)*r);
            break;
        }
        r++;
    }
}

char *
rx_replace(rx_t *re, const char *text, size_t len, const char *repl,
           int flags)
{
    sbuf out = { NULL, 0, 0, 0 };
    rx_match *m;
    int ng;
    long copied = 0;
    long pos = 0;
    long prev_end = -1;

    if (!re)
        return NULL;
    ng = re->ngroup + 1;
    m = malloc((size_t)ng * sizeof *m);
    if (!m)
        return NULL;

    while (pos <= (long)len) {
        int r = rx_exec(re, text, len, (size_t)pos, m, ng);

        if (r < 0) {            /* error (budget) */
            free(m);
            free(out.b);
            return NULL;
        }
        if (r == 0)
            break;

        /* Suppress an empty match abutting the previous match end. */
        if (m[0].so == m[0].eo && m[0].so == prev_end) {
            if (pos >= (long)len)
                break;
            pos++;
            continue;
        }

        sb_put(&out, text + copied, (size_t)(m[0].so - copied));
        expand(&out, repl, text, m, ng);
        copied = m[0].eo;
        prev_end = m[0].eo;

        if (m[0].eo > pos)
            pos = m[0].eo;
        else
            pos = m[0].eo + 1;  /* empty or non-advancing match */

        if (!(flags & RX_GLOBAL))
            break;
    }

    sb_put(&out, text + copied, (size_t)((long)len - copied));
    free(m);

    if (out.oom) {
        free(out.b);
        return NULL;
    }
    if (!out.b)                 /* empty result still returns a string */
        out.b = calloc(1, 1);
    else
        out.b[out.len] = '\0';
    return out.b;
}

/****************************************************************
 * Optional self-test harness: cc -DRX_TEST regex.c -o t && ./t
 ****************************************************************/

#ifdef RX_TEST
#include <stdio.h>
#include <assert.h>

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
#endif /* RX_TEST */

/****************************************************************
 * Optional sed-like CLI: cc -DRX_MAIN regex.c -o rsed
 *   rsed [-g] [-i] [-m] [-s] PATTERN REPLACEMENT < input
 ****************************************************************/

#ifdef RX_MAIN
#include <stdio.h>

int
main(int argc, char **argv)
{
    int flags = 0;
    int i = 1;
    const char *pat, *repl, *err;
    rx_t *re;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i] + 1;

        for (; *o; o++) {
            switch (*o) {
            case 'g': flags |= RX_GLOBAL; break;
            case 'i': flags |= RX_ICASE; break;
            case 'm': flags |= RX_MULTILINE; break;
            case 's': flags |= RX_DOTALL; break;
            default:
                fprintf(stderr, "unknown option -%c\n", *o);
                return 2;
            }
        }
    }
    if (argc - i < 2) {
        fprintf(stderr,
                "usage: %s [-gims] PATTERN REPLACEMENT < input\n", argv[0]);
        return 2;
    }
    pat = argv[i];
    repl = argv[i + 1];

    re = rx_compile(pat, flags, &err);
    if (!re) {
        fprintf(stderr, "bad pattern: %s\n", err);
        return 2;
    }

    while ((n = getline(&line, &cap, stdin)) >= 0) {
        int nl = (n > 0 && line[n - 1] == '\n');
        char *out;

        if (nl)
            line[--n] = '\0';
        out = rx_replace(re, line, (size_t)n, repl, flags);
        if (!out) {
            fprintf(stderr, "substitution failed\n");
            free(line);
            rx_free(re);
            return 1;
        }
        fputs(out, stdout);
        if (nl)
            putchar('\n');
        free(out);
    }

    free(line);
    rx_free(re);
    return 0;
}
#endif /* RX_MAIN */
