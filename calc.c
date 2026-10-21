// Aadityapratap Singh Baghel 241ADB122
// https://github.com/aadityapratap-dev/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic), conversation history: https://claude.ai/share/2670234a-0309-4c38-bbf2-7ca8fb874aad

// calc reads arithmetic expressions from text files and writes one result per
// line (or the position of the first error) into an output file.

// A strict c17 build hides some POSIX declarations (dirent, stat), this brings them back.
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// These end up in the output file names.
#define STUDENT_NAME "aadityapratap"
#define STUDENT_LASTNAME "baghel"
#define STUDENT_ID "241ADB122"
#define FILE_SUFFIX "_" STUDENT_NAME "_" STUDENT_LASTNAME "_" STUDENT_ID ".txt"

// Smallest long long, written out so no extra header is needed.
#define LL_MIN (-9223372036854775807LL - 1)

// ===== small helpers =====

static void *xrealloc(void *ptr, size_t n) {
    void *q = realloc(ptr, n);
    if (q == NULL) {
        fprintf(stderr, "calc: out of memory\n");
        exit(1);
    }
    return q;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// Results are collected here and written once at the end. If an error shows up
// on line 500 the file must contain only the error, so nothing can be written
// early. The buffer doubles when it is full, so adding is cheap and nothing is
// copied again and again, even for output of many megabytes.
typedef struct {
    char *d;
    size_t len;
    size_t cap;
} Buf;

static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) cap *= 2;
        b->d = xrealloc(b->d, cap);
        b->cap = cap;
    }
    memcpy(b->d + b->len, s, n);
    b->len += n;
}

// AI (Claude): prompt "read a whole file into memory in C without knowing its size"
// The spare byte makes s[len] writable, parse_primary may put a temporary NUL there.
static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = xrealloc(NULL, cap + 1);
    for (;;) {
        len += fread(buf + len, 1, cap - len, f);
        if (len < cap) break;  // a short read means end of file or an error
        cap *= 2;
        buf = xrealloc(buf, cap + 1);
    }
    if (ferror(f)) {
        int saved = errno;  // fclose and free could overwrite it
        fclose(f);
        free(buf);
        errno = saved;
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    *len_out = len;
    return buf;
}

// ===== values =====

// Every value remembers whether it is an integer or a float. That decides how
// the next operation is done: if either operand is a float the whole operation
// runs in double, otherwise it stays in exact 64 bit integers.
typedef struct {
    int is_float;
    long long i;
    double d;
} Value;

static Value mk_int(long long n) {
    Value v = {0, n, 0.0};
    return v;
}

static Value mk_float(double d) {
    Value v = {1, 0, d};
    return v;
}

static double as_double(Value v) { return v.is_float ? v.d : (double)v.i; }

// ===== parser state =====

// One operand of a chain like 2 ** 3 ** 2, see parse_power.
typedef struct {
    Value v;
    size_t start;  // where this operand begins in the file
    size_t op;     // where the ** in front of it begins
} PowItem;

// The parser never cuts tokens out of the text. It walks pos over the one big
// buffer, so pos is always "how many characters of the whole file we have
// passed", and every newline and every character of a comment line is counted
// without any extra work. An error position is simply pos + 1 (the task counts
// from 1) at the moment we find out a token is wrong.
typedef struct {
    char *s;      // the whole file
    size_t len;
    size_t pos;
    size_t err;   // 0 means no error yet, otherwise the 1 based position to report
    PowItem *chain;
    size_t chain_cap;
    size_t chain_top;
    size_t depth;  // how many opening brackets are currently open
} Parser;

// Every bracket level uses several nested function calls (expr, term, power,
// primary), so without a limit a line of endless ( would run out of stack.
#define MAX_DEPTH 4000

// Only the first error counts, everything after it is just unwinding.
static void fail(Parser *p, size_t idx) {
    if (p->err == 0) p->err = idx + 1;
}

static int at_eol(const Parser *p) { return p->pos >= p->len || p->s[p->pos] == '\n'; }

// Spaces and tabs are allowed between any two tokens, nothing else is.
static void skip_ws(Parser *p) {
    while (p->pos < p->len && (p->s[p->pos] == ' ' || p->s[p->pos] == '\t')) p->pos++;
}

// True when the next token is the ** operator. Both stars have to touch, so
// "* *" is two operators in a row (an error), not a power.
static int at_pow(Parser *p) {
    skip_ws(p);
    return p->pos + 1 < p->len && p->s[p->pos] == '*' && p->s[p->pos + 1] == '*';
}

// ===== arithmetic =====

// AI (Claude): prompt "fast integer power in C that cannot hit undefined behaviour on overflow"
// Squaring per exponent bit keeps 1 ** 1000000000 at about 30 steps, unsigned maths wraps.
static unsigned long long ipow(unsigned long long base, unsigned long long e) {
    unsigned long long result = 1;
    while (e) {
        if (e & 1) result *= base;
        e >>= 1;
        if (e) base *= base;
    }
    return result;
}

// op is plus, minus, times, divide or p for **. op_idx is where the operator
// starts and rhs_idx is where the right operand starts, because the task wants
// different errors at different places: division by zero and a negative
// exponent point at the right operand, since that operand is what is wrong,
// while a float result that is infinite or not a number points at the
// operator, since the operation as a whole produced it.
static Value apply(Parser *p, char op, Value a, Value b, size_t op_idx, size_t rhs_idx) {
    if (a.is_float || b.is_float) {
        // One float anywhere means the whole operation is done in double,
        // which is how 7 / 2.0 becomes 3.5 and 0.5 to the power of minus one becomes 2.
        double x = as_double(a), y = as_double(b), r = 0.0;
        switch (op) {
        case '+': r = x + y; break;
        case '-': r = x - y; break;
        case '*': r = x * y; break;
        case '/':
            if (y == 0.0) {
                fail(p, rhs_idx);
                return a;
            }
            r = x / y;
            break;
        default: r = pow(x, y); break;
        }
        // Overflow gives infinity and a negative number to the power 0.5 gives NaN.
        // Neither can be printed as a result, so they are errors.
        if (!isfinite(r)) {
            fail(p, op_idx);
            return a;
        }
        return mk_float(r);
    }
    // Both operands are integers here, so the exact integer rules apply.
    unsigned long long x = (unsigned long long)a.i, y = (unsigned long long)b.i;
    switch (op) {
    case '+': return mk_int((long long)(x + y));
    case '-': return mk_int((long long)(x - y));
    case '*': return mk_int((long long)(x * y));
    case '/':
        if (b.i == 0) {
            fail(p, rhs_idx);
            return a;
        }
        if (a.i == LL_MIN && b.i == -1) return a;  // the one division that overflows in C
        return mk_int(a.i / b.i);                  // C truncates toward zero, same as the task
    default:
        if (b.i < 0) {  // int ** negative int would not be an integer
            fail(p, rhs_idx);
            return a;
        }
        return mk_int((long long)ipow(x, y));
    }
}

static int format_value(char *out, size_t cap, Value v) {
    if (v.is_float) return snprintf(out, cap, "%.15g\n", v.d);
    return snprintf(out, cap, "%lld\n", v.i);
}

// ===== parser =====
// One function per precedence level, the tightest binding one at the bottom.
// Each level asks the next tighter level for its operands, so 3 + 2 * 5 groups
// as 3 + (2 * 5): the + level never sees the 2 on its own, the * level takes it.
//   expr    is term, then any number of plus or minus followed by a term
//   term    is power, then any number of times or divide followed by a power
//   power   is primary, optionally ** and more of the same
//   primary is a number or a bracketed expr

static Value parse_expr(Parser *p);

// A number or a bracketed expression. This is also where most syntax errors are
// noticed: any token that is neither a number nor an opening bracket is reported
// at its own start. That covers an invalid character, a second operator in a row
// ("7 + * 3") and a stray closing bracket ("2 * ()"). When the line ended
// instead, the error points at the newline (or one past the end of the file when
// the last line has no newline), which is what a missing operand or a missing
// closing bracket should report.
static Value parse_primary(Parser *p) {
    skip_ws(p);
    if (at_eol(p)) {
        fail(p, p->pos);
        return mk_int(0);
    }
    char c = p->s[p->pos];
    if (is_digit(c)) {
        size_t start = p->pos, end = start;
        while (end < p->len && is_digit(p->s[end])) end++;
        if (end < p->len && p->s[end] == '.') {
            // A float needs digits on both sides of the dot. "5." is an error at
            // the dot. (".5" never gets here, the dot itself is the bad token.)
            if (end + 1 >= p->len || !is_digit(p->s[end + 1])) {
                fail(p, end);
                return mk_int(0);
            }
            size_t fend = end + 1;
            while (fend < p->len && is_digit(p->s[fend])) fend++;
            // strtod would happily swallow an exponent like e3, which is not
            // allowed here. So the text is cut at fend for a moment by putting
            // a NUL there, instead of copying the digits somewhere else.
            char saved = p->s[fend];
            p->s[fend] = '\0';
            double d = strtod(p->s + start, NULL);
            p->s[fend] = saved;
            if (!isfinite(d)) {  // so many digits that it does not fit a double
                fail(p, start);
                return mk_int(0);
            }
            p->pos = fend;
            return mk_float(d);
        }
        // Digits that are not followed by a dot, plain integer. Anything that
        // follows (like the e in 1.5e3 or a letter) is left for the caller,
        // which reports it at its own position.
        unsigned long long n = 0;
        for (size_t k = start; k < end; k++) n = n * 10 + (unsigned long long)(p->s[k] - '0');
        p->pos = end;
        return mk_int((long long)n);
    }
    if (c == '(') {
        if (p->depth >= MAX_DEPTH) {
            fail(p, p->pos);
            return mk_int(0);
        }
        p->pos++;
        p->depth++;
        Value v = parse_expr(p);
        p->depth--;
        if (p->err) return v;
        skip_ws(p);
        // Anything except a closing bracket is wrong, at the end of the line
        // that is the missing bracket, otherwise it is the unexpected token.
        if (at_eol(p) || p->s[p->pos] != ')') {
            fail(p, p->pos);
            return v;
        }
        p->pos++;
        return v;
    }
    fail(p, p->pos);
    return mk_int(0);
}

static void chain_push(Parser *p, Value v, size_t start, size_t op) {
    if (p->chain_top == p->chain_cap) {
        p->chain_cap = p->chain_cap ? p->chain_cap * 2 : 16;
        p->chain = xrealloc(p->chain, p->chain_cap * sizeof(PowItem));
    }
    PowItem it = {v, start, op};
    p->chain[p->chain_top++] = it;
}

// ** is right associative: 2 ** 3 ** 2 has to be 2 ** (3 ** 2) = 512, the
// exponent is itself a whole chain. The chain is read first and then folded from
// the right, so the last pair is calculated first. A loop instead of a function
// that calls itself gives the same grouping, but a line with a million ** in a
// row cannot overflow the stack. The chain lives on one shared stack
// (chain_top) because brackets can start a new chain in the middle of this one.
static Value parse_power(Parser *p) {
    Value first = parse_primary(p);
    if (p->err || !at_pow(p)) return first;
    size_t base = p->chain_top;
    chain_push(p, first, 0, 0);
    while (at_pow(p)) {
        size_t op = p->pos;
        p->pos += 2;
        skip_ws(p);
        size_t start = p->pos;
        Value v = parse_primary(p);
        if (p->err) return v;
        chain_push(p, v, start, op);
    }
    size_t top = p->chain_top;
    Value acc = p->chain[top - 1].v;
    for (size_t k = top - 1; k > base; k--) {
        // chain[k].start is where the exponent begins, which is where a
        // negative exponent is reported.
        acc = apply(p, 'p', p->chain[k - 1].v, acc, p->chain[k].op, p->chain[k].start);
        if (p->err) break;
    }
    p->chain_top = base;
    return acc;
}

// * and / are left associative: 8 / 4 / 2 is (8 / 4) / 2. The loop does that,
// each new operand is combined with everything to its left.
static Value parse_term(Parser *p) {
    Value l = parse_power(p);
    while (!p->err) {
        skip_ws(p);
        if (p->pos >= p->len) break;
        char c = p->s[p->pos];
        if (c != '*' && c != '/') break;
        size_t op = p->pos;
        p->pos++;
        skip_ws(p);
        size_t rhs = p->pos;
        Value r = parse_power(p);
        if (p->err) break;
        l = apply(p, c, l, r, op, rhs);
    }
    return l;
}

// Same loop for plus and minus, one level looser than term.
static Value parse_expr(Parser *p) {
    Value l = parse_term(p);
    while (!p->err) {
        skip_ws(p);
        if (p->pos >= p->len) break;
        char c = p->s[p->pos];
        if (c != '+' && c != '-') break;
        size_t op = p->pos;
        p->pos++;
        skip_ws(p);
        size_t rhs = p->pos;
        Value r = parse_term(p);
        if (p->err) break;
        l = apply(p, c, l, r, op, rhs);
    }
    return l;
}

// Evaluates the whole file. The file is one flat buffer that is read once from
// start to end and never copied, so size and line length do not matter.
static void run_file(char *text, size_t len, Buf *out) {
    Parser p = {text, len, 0, 0, NULL, 0, 0, 0};
    while (p.pos < p.len) {
        skip_ws(&p);
        if (at_eol(&p)) {  // blank line, nothing to output
            if (p.pos < p.len) p.pos++;
            continue;
        }
        if (p.s[p.pos] == '#') {  // comment line, only counts for positions
            while (p.pos < p.len && p.s[p.pos] != '\n') p.pos++;
            if (p.pos < p.len) p.pos++;
            continue;
        }
        Value v = parse_expr(&p);
        if (!p.err) {
            // The expression must use up the whole line. What is left over is an
            // extra closing bracket, a stray character or a # that is not at the
            // start of the line, all reported where they start.
            skip_ws(&p);
            if (!at_eol(&p)) fail(&p, p.pos);
        }
        if (p.err) break;
        char tmp[64];
        int n = format_value(tmp, sizeof tmp, v);
        buf_add(out, tmp, (size_t)n);
        if (p.pos < p.len) p.pos++;  // step over the newline
    }
    if (p.err) {
        // No partial results: throw away what was collected so far.
        out->len = 0;
        char tmp[64];
        int n = snprintf(tmp, sizeof tmp, "ERROR:%llu\n", (unsigned long long)p.err);
        buf_add(out, tmp, (size_t)n);
    }
    free(p.chain);
}

// ===== files and command line =====

// Last part of a path. strip_txt removes a trailing .txt. Trailing slashes are
// ignored so "labs/" gives "labs".
static char *make_base(const char *path, int strip_txt) {
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/') end--;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t n = end - start;
    if (strip_txt && n > 4 && memcmp(path + end - 4, ".txt", 4) == 0) n -= 4;
    char *out = xrealloc(NULL, n + 1);
    memcpy(out, path + start, n);
    out[n] = '\0';
    return out;
}

// Makes the output folder if it is missing (its parent is expected to exist).
static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        fprintf(stderr, "calc: '%s' exists and is not a folder\n", path);
        return -1;
    }
    if (mkdir(path, 0777) != 0) {
        fprintf(stderr, "calc: cannot create folder '%s': %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int write_file(const char *path, const Buf *b) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "calc: cannot write '%s': %s\n", path, strerror(errno));
        return -1;
    }
    int ok = b->len == 0 || fwrite(b->d, 1, b->len, f) == b->len;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "calc: error while writing '%s'\n", path);
        return -1;
    }
    return 0;
}

// Evaluates one input file and writes <out_dir>/<base>_<name>_<lastname>_<id>.txt
static int process_file(const char *in_path, const char *out_dir) {
    size_t len = 0;
    char *text = read_file(in_path, &len);
    if (text == NULL) {
        fprintf(stderr, "calc: cannot read '%s': %s\n", in_path, strerror(errno));
        return -1;
    }
    Buf out = {NULL, 0, 0};
    run_file(text, len, &out);
    free(text);

    int rc = ensure_dir(out_dir);
    if (rc == 0) {
        char *base = make_base(in_path, 1);
        int n = snprintf(NULL, 0, "%s/%s" FILE_SUFFIX, out_dir, base);
        char *path = xrealloc(NULL, (size_t)n + 1);
        snprintf(path, (size_t)n + 1, "%s/%s" FILE_SUFFIX, out_dir, base);
        rc = write_file(path, &out);
        free(path);
        free(base);
    }
    free(out.d);
    return rc;
}

// Joins a folder and a file name with exactly one slash between them.
static char *join_path(const char *dir, const char *name) {
    size_t n = strlen(dir);
    const char *sep = (n > 0 && dir[n - 1] == '/') ? "" : "/";
    int len = snprintf(NULL, 0, "%s%s%s", dir, sep, name);
    char *out = xrealloc(NULL, (size_t)len + 1);
    snprintf(out, (size_t)len + 1, "%s%s%s", dir, sep, name);
    return out;
}

static int ends_with_txt(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".txt") == 0;
}

static int cmp_paths(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// Evaluates every *.txt file directly inside dir. Other files and sub folders
// are skipped (stat, not the name, decides, so a folder called x.txt is skipped
// too). All paths are collected before anything is written, so result files
// that land in the same folder are never picked up as input in this run.
// Sorting makes the order the same on every machine, readdir has no order.
static int process_dir(const char *dir, const char *out_dir) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "calc: cannot open folder '%s': %s\n", dir, strerror(errno));
        return -1;
    }
    char **paths = NULL;
    size_t count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!ends_with_txt(e->d_name)) continue;
        char *full = join_path(dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(full);
            continue;
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 16;
            paths = xrealloc(paths, cap * sizeof *paths);
        }
        paths[count++] = full;
    }
    closedir(d);
    if (count > 1) qsort(paths, count, sizeof *paths, cmp_paths);

    int rc = ensure_dir(out_dir);
    for (size_t k = 0; k < count; k++) {
        // One bad file should not stop the others, but the exit code must say
        // that something went wrong.
        if (rc == 0 && process_file(paths[k], out_dir) != 0) rc = -1;
        free(paths[k]);
    }
    free(paths);
    return rc;
}

static void usage(void) {
    fprintf(stderr, "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

int main(int argc, char **argv) {
    const char *dir = NULL, *out_dir = NULL, *input = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int is_dir_opt = strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0;
        int is_out_opt = strcmp(a, "-o") == 0 || strcmp(a, "--output-dir") == 0;
        if (is_dir_opt || is_out_opt) {
            if (i + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs a value\n", a);
                usage();
                return 2;
            }
            if (is_dir_opt) dir = argv[++i];
            else out_dir = argv[++i];
        } else if (a[0] == '-') {
            fprintf(stderr, "calc: unknown option '%s'\n", a);
            usage();
            return 2;
        } else if (input != NULL) {
            fprintf(stderr, "calc: only one input file is allowed\n");
            usage();
            return 2;
        } else {
            input = a;
        }
    }
    // Exactly one of a folder or a single file is needed. This also catches the
    // call with no arguments at all.
    if ((dir == NULL) == (input == NULL)) {
        usage();
        return 2;
    }

    // Without an output folder given, use <input_base>_<name>_<studentid> in the
    // current folder. For a single file the base has no .txt, for a folder it is
    // the folder's own name.
    char *default_out = NULL;
    if (out_dir == NULL) {
        char *base = make_base(dir != NULL ? dir : input, dir == NULL);
        int n = snprintf(NULL, 0, "%s_" STUDENT_NAME "_" STUDENT_ID, base);
        default_out = xrealloc(NULL, (size_t)n + 1);
        snprintf(default_out, (size_t)n + 1, "%s_" STUDENT_NAME "_" STUDENT_ID, base);
        free(base);
        out_dir = default_out;
    }

    int rc = dir != NULL ? process_dir(dir, out_dir) : process_file(input, out_dir);
    free(default_out);
    return rc == 0 ? 0 : 1;
}
