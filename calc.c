// Aadityapratap Singh Baghel 241ADB122
// https://github.com/aadityapratap-dev/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: K&R "The C Programming Language" 2nd ed. chapter 5 (pointers and walking through chars), Aho/Lam/Sethi/Ullman "Compilers: Principles, Techniques, and Tools" 2nd ed. section 2.4 (recursive descent). Details in the comments where I used them.

// strict c17 hides some posix stuff (mkdir for now), this brings it back
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// these go into the output file names
#define STUDENT_NAME "aadityapratap"
#define STUDENT_LASTNAME "baghel"
#define STUDENT_ID "241ADB122"

enum { T_NUM, T_PLUS, T_MINUS, T_EOL, T_BAD };

struct token {
    int kind;
    size_t start;   // 0 based index into the file text
    long long val;
};

static const char *src;     // the whole file, with one extra \n after the end
static size_t src_len;
static size_t scan;         // where the tokenizer is, never reset between lines
static struct token cur;    // the token the parser is looking at
static size_t err_pos;      // 1 based, 0 means no error yet

static char *out;
static size_t out_len, out_cap;

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
    scan++;
    if (c >= '0' && c <= '9') {
        cur.kind = T_NUM;
        cur.val = c - '0';
        while (src[scan] >= '0' && src[scan] <= '9') {
            cur.val = cur.val * 10 + (src[scan] - '0');
            scan++;
        }
    } else if (c == '+') {
        cur.kind = T_PLUS;
    } else if (c == '-') {
        cur.kind = T_MINUS;
    } else {
        cur.kind = T_BAD;
    }
}

static long long primary(void)
{
    long long v = 0;
    if (cur.kind == T_NUM) {
        v = cur.val;
        next_token();
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

// expr := primary { ('+' | '-') primary }
// the loop makes it left associative, 10 - 3 + 1 is (10 - 3) + 1
static long long expr(void)
{
    long long lhs = primary();
    while (!err_pos && (cur.kind == T_PLUS || cur.kind == T_MINUS)) {
        int op = cur.kind;
        next_token();
        long long rhs = primary();
        if (op == T_PLUS)
            lhs += rhs;
        else
            lhs -= rhs;
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
        if (src[scan] == '\n') {
            // blank line, no output for it
            line = scan + 1;
            continue;
        }

        next_token();
        long long v = expr();
        // expr stopped on something that isn't an operator, so this is leftover
        // junk like a bad char after a complete number. blame that token
        if (!err_pos && cur.kind != T_EOL)
            fail(cur.start);
        if (err_pos)
            return;

        char tmp[32];
        int n = snprintf(tmp, sizeof tmp, "%lld\n", v);
        out_add(tmp, n);
        line = cur.start + 1;   // cur is the \n here, next line starts after it
    }
}

int main(int argc, char **argv)
{
    const char *outdir = NULL;
    const char *input = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outdir = argv[++i];
        else
            input = argv[i];
    }
    if (input == NULL || outdir == NULL) {
        fprintf(stderr, "usage: calc -o OUTDIR input.txt\n");
        return 1;
    }

    char *text = read_file(input, &src_len);
    if (text == NULL) {
        fprintf(stderr, "can't open %s\n", input);
        return 1;
    }
    // read_file only stops when n < cap, so this spare byte is always there.
    // having a \n at the very end means a last line without one still ends
    // properly, and its end of line position comes out as file length + 1
    text[src_len] = '\n';
    src = text;

    run();
    if (err_pos) {
        // an error anywhere means no partial results, throw away what we had
        char tmp[32];
        int n = snprintf(tmp, sizeof tmp, "ERROR:%zu\n", err_pos);
        out_len = 0;
        out_add(tmp, n);
    }

    // task1.txt becomes task1_<name>_<lastname>_<id>.txt
    const char *base = strrchr(input, '/');
    base = base ? base + 1 : input;
    size_t blen = strlen(base);
    if (blen > 4 && strcmp(base + blen - 4, ".txt") == 0)
        blen -= 4;

    if (mkdir(outdir, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "can't create %s\n", outdir);
        return 1;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/%.*s_" STUDENT_NAME "_" STUDENT_LASTNAME "_" STUDENT_ID ".txt",
             outdir, (int)blen, base);

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "can't write %s\n", path);
        return 1;
    }
    fwrite(out, 1, out_len, fp);
    if (fclose(fp) != 0) {
        fprintf(stderr, "write to %s failed\n", path);
        return 1;
    }

    free(out);
    free(text);
    return 0;
}
