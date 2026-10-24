// Aadityapratap Singh Baghel 241ADB122
// https://github.com/aadityapratap-dev/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: K&R "The C Programming Language" 2nd ed. chapter 5 (pointers and walking through chars), Aho/Lam/Sethi/Ullman "Compilers: Principles, Techniques, and Tools" 2nd ed. section 2.4 (recursive descent). Details in the comments where I used them.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: calc input.txt\n");
        return 1;
    }

    size_t len;
    char *text = read_file(argv[1], &len);
    if (text == NULL) {
        fprintf(stderr, "can't open %s\n", argv[1]);
        return 1;
    }

    // nothing is evaluated yet, just checking the read works
    (void)len;

    free(text);
    return 0;
}
