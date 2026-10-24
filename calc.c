// Aadityapratap Singh Baghel 241ADB122
// https://github.com/aadityapratap-dev/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: K&R "The C Programming Language" 2nd ed. chapter 5 (pointers and walking through chars), Aho/Lam/Sethi/Ullman "Compilers: Principles, Techniques, and Tools" 2nd ed. section 2.4 (recursive descent). Details in the comments where I used them.

// strict c17 hides some posix stuff (mkdir, dirent, stat), this brings it back
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// these go into the output file names
#define STUDENT_NAME "aadityapratap"
#define STUDENT_LASTNAME "baghel"
#define STUDENT_ID "241ADB122"

enum { T_NUM, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW, T_LP, T_RP, T_EOL, T_BAD };

// a number is either an int or a double, is_float says which field to use
struct num {
    int is_float;
    long long i;
    double f;
};

struct token {
    int kind;
    size_t start;   // 0 based index into the file text
    struct num val;
};

static const char *src;     // the whole file, with one extra \n after the end
static size_t src_len;
static size_t scan;         // where the tokenizer is, never reset between lines
static struct token cur;    // the token the parser is looking at
static size_t err_pos;      // 1 based, 0 means no error yet

static char *out;
static size_t out_len, out_cap;

static char *numbuf;        // scratch copy of a float token for strtod
static size_t numcap;

// reads the whole file into one malloc'd buffer, returns NULL if it can't be opened
static char *read_file(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return NULL;

    size_t cap = 4096;
    size_t n = 0;
    char *buf = malloc(cap);
    if (buf == NULL)
        exit(1);

    // doubling idea from K&R, so the copying adds up to O(size) and not O(size^2)
    while (1) {
        size_t got = fread(buf + n, 1, cap - n, fp);
        n += got;
        if (n < cap)
            break;
        cap *= 2;
        buf = realloc(buf, cap);
        if (buf == NULL)
            exit(1);
    }
    fclose(fp);
    *len = n;
    return buf;
}

// whole output is built up in memory first. if an error shows up later in the
// file nothing is written except the ERROR line, so I can't stream it out
static void out_add(const char *s, size_t n)
{
    if (out_cap == 0)
        out_cap = 4096;
    while (out_len + n > out_cap)
        out_cap *= 2;
    out = realloc(out, out_cap);
    if (out == NULL)
        exit(1);
    memcpy(out + out_len, s, n);
    out_len += n;
}

// only the first error counts, anything after it is a side effect of the first
static void fail(size_t at)
{
    if (err_pos == 0)
        err_pos = at + 1;
}

// strtod needs a \0 at the end of the number and the file text has none there,
// so copy the token into a scratch buffer. it is reused, so no malloc per number.
// I can't just call strtod on src directly, it would swallow the e in 1.5e3
static struct num to_float(size_t from, size_t to)
{
    size_t n = to - from;
    if (n + 1 > numcap) {
        numcap = n + 1;
        numbuf = realloc(numbuf, numcap);
        if (numbuf == NULL)
            exit(1);
    }
    memcpy(numbuf, src + from, n);
    numbuf[n] = '\0';

    struct num r;
    r.is_float = 1;
    r.i = 0;
    r.f = strtod(numbuf, NULL);
    return r;
}

static double as_double(struct num n)
{
    return n.is_float ? n.f : (double)n.i;
}

static struct num make_float(double f)
{
    struct num r;
    r.is_float = 1;
    r.i = 0;
    r.f = f;
    return r;
}

static struct num make_int(long long i)
{
    struct num r;
    r.is_float = 0;
    r.i = i;
    r.f = 0;
    return r;
}

// scan counts every char of the file from 0, including the \n at the end of
// each line, and it is never reset when a new line starts. so start + 1 is
// already the position the task wants (1 based, \n counted)
static void next_token(void)
{
    while (src[scan] == ' ' || src[scan] == '\t')
        scan++;
    cur.start = scan;

    char c = src[scan];
    if (c == '\n') {
        // the \n is not eaten, it stays as the end of line token
        cur.kind = T_EOL;
        return;
    }
    if (c >= '0' && c <= '9') {
        cur.kind = T_NUM;
        while (src[scan] >= '0' && src[scan] <= '9')
            scan++;
        // it is only a float when there are digits on both sides of the dot.
        // so 5. and .5 never become floats here, the 5 (or nothing) is a number
        // and the leftover . is a bad char token on the next call, which puts
        // the error at the dot. same for the e in 1.5e3
        if (src[scan] == '.' && src[scan + 1] >= '0' && src[scan + 1] <= '9') {
            scan++;
            while (src[scan] >= '0' && src[scan] <= '9')
                scan++;
            cur.val = to_float(cur.start, scan);
        } else {
            cur.val.is_float = 0;
            cur.val.i = 0;
            for (size_t k = cur.start; k < scan; k++)
                cur.val.i = cur.val.i * 10 + (src[k] - '0');
        }
        return;
    }

    scan++;
    if (c == '+') {
        cur.kind = T_PLUS;
    } else if (c == '-') {
        cur.kind = T_MINUS;
    } else if (c == '*') {
        // ** is one token, so look one char ahead. "* *" with a space stays two
        // separate stars and that is an error later, which is what we want
        if (src[scan] == '*') {
            scan++;
            cur.kind = T_POW;
        } else {
            cur.kind = T_STAR;
        }
    } else if (c == '/') {
        cur.kind = T_SLASH;
    } else if (c == '(') {
        cur.kind = T_LP;
    } else if (c == ')') {
        cur.kind = T_RP;
    } else {
        cur.kind = T_BAD;
    }
}

static struct num expr(void);

static struct num primary(void)
{
    struct num v = make_int(0);
    if (cur.kind == T_NUM) {
        v = cur.val;
        next_token();
    } else if (cur.kind == T_LP) {
        // a ( starts a whole new expr, which is how the nesting works. expr
        // calls down to primary, primary calls expr again for what is inside
        next_token();
        v = expr();
        if (!err_pos) {
            if (cur.kind == T_RP)
                next_token();
            else
                // expr gave up on something that isn't ), most of the time
                // that is the \n of the line, so missing ) lands on the \n
                fail(cur.start);
        }
    } else {
        // not a number where one has to be. this covers all three cases at once:
        // a bad char (cur is the T_BAD token, so its own position), two operators
        // in a row (cur is the second one) and a missing operand (cur is the \n,
        // or file length + 1 for the last line). in every case cur.start is the
        // token the parser got stuck on, which is exactly what the task wants
        fail(cur.start);
    }
    return v;
}

// int or float? every operation looks at both operands. if either one is a
// float the whole thing is done in double, otherwise it stays long long math.
// that is why 7 / 2 is 3 but 7 / 2.0 is 3.5. a float never goes back to int
//
// for float results the check is the same everywhere: if the answer is inf or
// nan the operator is blamed, because the operator is what blew it up (the
// operands were fine, 10.0 and 400 are both ok on their own)

// power := primary [ '**' power ]
// no loop here on purpose. after the ** this calls power() again for the
// exponent, so 2 ** 3 ** 2 groups as 2 ** (3 ** 2) = 512. a loop like in expr
// would group it to the left, (2 ** 3) ** 2 = 64, which is wrong
static struct num power(void)
{
    struct num base = primary();
    if (err_pos || cur.kind != T_POW)
        return base;

    size_t op_at = cur.start;
    next_token();
    size_t exp_at = cur.start;
    struct num e = power();
    if (err_pos)
        return base;

    if (base.is_float || e.is_float) {
        // pow() gives nan for stuff like (0 - 8) ** 0.5 and inf when it gets
        // too big, both land on the operator
        double r = pow(as_double(base), as_double(e));
        if (!isfinite(r)) {
            fail(op_at);
            return base;
        }
        return make_float(r);
    }

    if (e.i < 0) {
        // two ints and a negative exponent. the exponent is what's wrong, so
        // point at the start of it (saved before the call, because by now cur
        // has moved on to whatever comes after the exponent)
        fail(exp_at);
        return base;
    }

    // square and multiply. the b *= b is skipped on the last round so it
    // can't overflow for no reason
    long long b = base.i;
    long long n = e.i;
    long long r = 1;
    while (n > 0) {
        if (n & 1)
            r *= b;
        n >>= 1;
        if (n > 0)
            b *= b;
    }
    return make_int(r);
}

// term := power { ('*' | '/') power }
// same loop trick as expr below, so 8 / 4 / 2 is (8 / 4) / 2
static struct num term(void)
{
    struct num lhs = power();
    while (!err_pos && (cur.kind == T_STAR || cur.kind == T_SLASH)) {
        int op = cur.kind;
        size_t op_at = cur.start;
        next_token();
        size_t rhs_at = cur.start;
        struct num rhs = power();
        if (err_pos)
            break;

        int is_float = lhs.is_float || rhs.is_float;
        if (op == T_SLASH && (is_float ? as_double(rhs) == 0.0 : rhs.i == 0)) {
            // the 0 is the problem, not the slash, so point at where the right
            // operand starts. rhs_at is saved before parsing it because by the
            // time we get here cur has moved on to the next token
            fail(rhs_at);
            break;
        }

        if (is_float) {
            double a = as_double(lhs);
            double b = as_double(rhs);
            lhs = make_float(op == T_STAR ? a * b : a / b);
            if (!isfinite(lhs.f))
                fail(op_at);
        } else if (op == T_STAR) {
            lhs = make_int(lhs.i * rhs.i);
        } else {
            lhs = make_int(lhs.i / rhs.i);  // C already truncates toward zero, which is what we need
        }
    }
    return lhs;
}

// expr := term { ('+' | '-') term }
// expr calls term, term calls power, power calls primary. each level only sees
// the results of the level under it, so ** is glued together first, then * and /,
// and + and - get what is left. that is the whole reason for the precedence.
// 3 + 2 * 5 is 3 + (2 * 5)
static struct num expr(void)
{
    struct num lhs = term();
    while (!err_pos && (cur.kind == T_PLUS || cur.kind == T_MINUS)) {
        int op = cur.kind;
        size_t op_at = cur.start;
        next_token();
        struct num rhs = term();
        if (err_pos)
            break;

        if (lhs.is_float || rhs.is_float) {
            double a = as_double(lhs);
            double b = as_double(rhs);
            lhs = make_float(op == T_PLUS ? a + b : a - b);
            if (!isfinite(lhs.f))
                fail(op_at);
        } else if (op == T_PLUS) {
            lhs = make_int(lhs.i + rhs.i);
        } else {
            lhs = make_int(lhs.i - rhs.i);
        }
    }
    return lhs;
}

static void run(void)
{
    size_t line = 0;    // index where the current line starts

    while (line < src_len) {
        scan = line;
        while (src[scan] == ' ' || src[scan] == '\t')
            scan++;
        // a # as the first thing on the line makes it a comment. skipping to the
        // \n by moving scan means the comment chars still get counted for positions
        if (src[scan] == '#') {
            while (src[scan] != '\n')
                scan++;
        }
        if (src[scan] == '\n') {
            // blank line or comment, no output for it
            line = scan + 1;
            continue;
        }

        next_token();
        struct num v = expr();
        // expr stopped on something that isn't an operator, so this is leftover
        // junk like a bad char after a complete number. blame that token.
        // an extra ) also ends up here, nothing was open for it so primary never
        // ate it, and it gets reported at its own position
        if (!err_pos && cur.kind != T_EOL)
            fail(cur.start);
        if (err_pos)
            return;

        char tmp[64];
        int n;
        if (v.is_float)
            n = snprintf(tmp, sizeof tmp, "%.15g\n", v.f);
        else
            n = snprintf(tmp, sizeof tmp, "%lld\n", v.i);
        out_add(tmp, n);
        line = cur.start + 1;   // cur is the \n here, next line starts after it
    }
}

// pointer to the file name part of a path, len is its length without a .txt
static const char *file_stem(const char *path, size_t *len)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    *len = strlen(base);
    if (*len > 4 && strcmp(base + *len - 4, ".txt") == 0)
        *len -= 4;
    return base;
}

// same idea for a folder, "labs" and "labs/" both have to give labs
static const char *dir_stem(const char *path, size_t *len)
{
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/')
        end--;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/')
        start--;
    *len = end - start;
    return path + start;
}

static int make_dir(const char *path)
{
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "can't create folder %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

// evaluates one file and writes <outdir>/<stem>_<name>_<lastname>_<id>.txt
// the out buffer and the error state are shared, so they get reset first
static int do_file(const char *input, const char *outdir)
{
    size_t len;
    char *text = read_file(input, &len);
    if (text == NULL) {
        fprintf(stderr, "can't open %s: %s\n", input, strerror(errno));
        return 1;
    }
    // read_file only stops when n < cap, so this spare byte is always there.
    // having a \n at the very end means a last line without one still ends
    // properly, and its end of line position comes out as file length + 1
    text[len] = '\n';
    src = text;
    src_len = len;
    scan = 0;
    err_pos = 0;
    out_len = 0;

    run();
    if (err_pos) {
        // an error anywhere means no partial results, throw away what we had
        char tmp[32];
        int n = snprintf(tmp, sizeof tmp, "ERROR:%zu\n", err_pos);
        out_len = 0;
        out_add(tmp, n);
    }
    free(text);

    size_t blen;
    const char *base = file_stem(input, &blen);
    char path[4096];
    snprintf(path, sizeof path, "%s/%.*s_" STUDENT_NAME "_" STUDENT_LASTNAME "_" STUDENT_ID ".txt",
             outdir, (int)blen, base);

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "can't write %s: %s\n", path, strerror(errno));
        return 1;
    }
    fwrite(out, 1, out_len, fp);
    if (fclose(fp) != 0) {
        fprintf(stderr, "writing %s failed\n", path);
        return 1;
    }
    return 0;
}

static int do_dir(const char *dir, const char *outdir)
{
    DIR *dp = opendir(dir);
    if (dp == NULL) {
        fprintf(stderr, "can't open folder %s: %s\n", dir, strerror(errno));
        return 1;
    }
    if (make_dir(outdir)) {
        closedir(dp);
        return 1;
    }

    int rc = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n <= 4 || strcmp(de->d_name + n - 4, ".txt") != 0)
            continue;

        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        // only plain files directly inside, a folder called x.txt doesn't count
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (do_file(path, outdir))
            rc = 1;
    }
    closedir(dp);
    return rc;
}

static int usage(void)
{
    fprintf(stderr, "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
    return 1;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    const char *outdir = NULL;
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0) {
            if (++i >= argc)
                return usage();
            dir = argv[i];
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--output-dir") == 0) {
            if (++i >= argc)
                return usage();
            outdir = argv[i];
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "unknown option %s\n", a);
            return usage();
        } else if (input != NULL) {
            return usage();
        } else {
            input = a;
        }
    }
    // exactly one of -d and a file name
    if ((dir == NULL) == (input == NULL))
        return usage();

    // no -o: <input_base>_<name>_<id> in the current folder
    char defdir[4096];
    if (outdir == NULL) {
        size_t blen;
        const char *base = dir ? dir_stem(dir, &blen) : file_stem(input, &blen);
        snprintf(defdir, sizeof defdir, "%.*s_" STUDENT_NAME "_" STUDENT_ID, (int)blen, base);
        outdir = defdir;
    }

    int rc;
    if (dir != NULL) {
        rc = do_dir(dir, outdir);
    } else {
        // a missing input should fail before we make an empty output folder
        FILE *probe = fopen(input, "rb");
        if (probe == NULL) {
            fprintf(stderr, "can't open %s: %s\n", input, strerror(errno));
            return 1;
        }
        fclose(probe);
        rc = make_dir(outdir) || do_file(input, outdir);
    }

    free(out);
    free(numbuf);
    return rc;
}
