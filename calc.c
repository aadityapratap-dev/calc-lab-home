// Aadityapratap Singh Baghel 241ADB122
// https://github.com/aadityapratap-dev/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: K&R "The C Programming Language" 2nd ed. chapter 4.3 (the calculator example, reading chars one at a time), Aho/Lam/Sethi/Ullman "Compilers: Principles, Techniques, and Tools" 2nd ed. section 2.4 (recursive descent). Details in the comments where I used them.

// strict c17 hides some posix stuff (mkdir, dirent, stat), this brings it back
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STUDENT_NAME "aadityapratap"
#define STUDENT_LASTNAME "baghel"
#define STUDENT_ID "241ADB122"

enum { T_NUM, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW, T_LP, T_RP, T_EOL, T_BAD };

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
static struct token cur;
static size_t err_pos;      // 1 based, 0 means no error yet

static char *out;
static size_t out_len, out_cap;

static char *numbuf;        // scratch copy of a float token for strtod
static size_t numcap;

// whole file into one buffer, NULL if it won't open. read once and everything
// after works on it by index, nothing gets copied per line
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

// results stay in memory until the file is done, an error late in the file
// throws all of them away
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

static void fail(size_t at)
{
    if (err_pos == 0)
        err_pos = at + 1;
}

// strtod straight on src would eat the e in 1.5e3, so copy the token first
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

// Tokenizer structure based on K&R chapter 4.3, one char at a time.
//   the positions part is my own: scan is never reset between lines and the \n
//   counts, so start + 1 is the position
static void next_token(void)
{
    while (src[scan] == ' ' || src[scan] == '\t')
        scan++;
    cur.start = scan;

    char c = src[scan];
    if (c == '\n') {
        // \n is left alone, nothing moves past it
        cur.kind = T_EOL;
        return;
    }
    if (c >= '0' && c <= '9') {
        cur.kind = T_NUM;
        while (src[scan] >= '0' && src[scan] <= '9')
            scan++;
        // 5. and 1.5e3 stop early, the leftover . or e comes out as T_BAD
        // next call and the error lands on it
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
        // "* *" with a space is two stars, only ** together is a power
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
        next_token();
        v = expr();
        if (!err_pos) {
            if (cur.kind == T_RP)
                next_token();
            else
                // usually the \n
                fail(cur.start);
        }
    } else {
        // bad char, second operator and missing operand all come here, cur is
        // whatever broke it. errors always go at the token the parser got stuck on
        fail(cur.start);
    }
    return v;
}

// int or float: one float operand and the whole operation is double, 7 / 2 is
// 3 but 7 / 2.0 is 3.5. it never goes back to int. if a float result comes out
// inf or nan the operator gets blamed, same check in expr and term too

static struct num do_power(struct num base, struct num e, size_t op_at, size_t exp_at)
{
    if (base.is_float || e.is_float) {
        // (0 - 8) ** 0.5 is nan
        double r = pow(as_double(base), as_double(e));
        if (!isfinite(r)) {
            fail(op_at);
            return base;
        }
        return make_float(r);
    }

    if (e.i < 0) {
        // points at the exponent, exp_at was saved before it got parsed
        fail(exp_at);
        return base;
    }

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

// stack, because a ( in the middle of a chain starts another power()
struct pow_item {
    struct num val;
    size_t op_at;   // the ** in front of this operand (not used for the first)
    size_t at;      // where the operand starts
};
static struct pow_item *chain;
static size_t chain_len, chain_cap;

static void chain_push(struct num val, size_t op_at, size_t at)
{
    if (chain_len == chain_cap) {
        chain_cap = chain_cap ? chain_cap * 2 : 64;
        chain = realloc(chain, chain_cap * sizeof *chain);
        if (chain == NULL)
            exit(1);
    }
    chain[chain_len].val = val;
    chain[chain_len].op_at = op_at;
    chain[chain_len].at = at;
    chain_len++;
}

// power := primary [ '**' power ]
// first version really did call power() for the exponent, like the grammar says.
// then a test line with 200000 ** segfaulted (stack). so now the operands go
// into chain and get combined from the right end, same grouping, 2 ** 3 ** 2
// is 2 ** (3 ** 2) = 512
static struct num power(void)
{
    size_t first = chain_len;
    size_t at = cur.start;
    chain_push(primary(), 0, at);

    while (!err_pos && cur.kind == T_POW) {
        size_t op_at = cur.start;
        next_token();
        at = cur.start;
        struct num v = primary();
        chain_push(v, op_at, at);
    }

    struct num acc = chain[chain_len - 1].val;
    // chain[k] is the exponent, chain[k - 1] its base
    for (size_t k = chain_len - 1; !err_pos && k > first; k--)
        acc = do_power(chain[k - 1].val, acc, chain[k].op_at, chain[k].at);

    chain_len = first;
    return acc;
}

// term := power { ('*' | '/') power }
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
            // blame the right operand, rhs_at saved before power() moved cur on
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
            lhs = make_int(lhs.i / rhs.i);
        }
    }
    return lhs;
}

// expr := term { ('+' | '-') term }
// Recursive descent, one function per level, from Aho et al. section 2.4.
//   the while loops instead of left recursion are my own.
// expr calls term, term calls power, so + and - get whatever * / ** left over,
// that's why 3 + 2 * 5 is 3 + (2 * 5)
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
        // comment, scan moves over it so the chars still count
        if (src[scan] == '#') {
            while (src[scan] != '\n')
                scan++;
        }
        if (src[scan] == '\n') {
            line = scan + 1;
            continue;
        }

        next_token();
        struct num v = expr();
        // something left over, an extra ) ends up here too
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

static const char *file_stem(const char *path, size_t *len)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    *len = strlen(base);
    if (*len > 4 && strcmp(base + *len - 4, ".txt") == 0)
        *len -= 4;
    return base;
}

// labs and labs/ both give labs
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

static int do_file(const char *input, const char *outdir)
{
    size_t len;
    char *text = read_file(input, &len);
    if (text == NULL) {
        fprintf(stderr, "can't open %s: %s\n", input, strerror(errno));
        return 1;
    }
    // there is always a spare byte here. the extra \n means a last line without
    // one still ends, and its position comes out as file length + 1
    text[len] = '\n';
    src = text;
    src_len = len;
    scan = 0;
    err_pos = 0;
    out_len = 0;

    run();
    if (err_pos) {
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
        // a folder called x.txt doesn't count
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
    free(chain);
    return rc;
}
