/* The C library of wasm-apps/sdk/libc: its state is what the functions gave,
 * and its printf, puts and putchar lines go to the app's log. The struct
 * copy and the zeroed array make clang call memcpy and memset itself. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fc_app.h"

FC_APP_ABI()

struct big {
    char text[200];
    int n;
};
static struct big s_big = {"copied", 7};

extern unsigned char __heap_base; /* the end of the app's memory, once WAMR shrinks it */

/* Every other function of the library: a bit set for each check where it
 * did not give what C says, 0 if all did. */
static int others(void)
{
    char a[16] = "hello", b[16];
    int bit = 1, bad = 0;
#define CHECK(c) (bad |= (c) ? 0 : bit, bit <<= 1)
    CHECK(memcmp(a, "help", 3) == 0 && memcmp(a, "help", 4) < 0);
    memmove(a + 1, a, 5); /* overlapping: "hhello" */
    CHECK(strcmp(a, "hhello") == 0);
    CHECK(memchr(a, 'e', 6) == a + 2 && strchr(a, 'l') == a + 3);
    CHECK(strcpy(b, "abc") == b && strcmp(b, "abc") == 0);
    CHECK(strncmp("abcd", "abcx", 3) == 0 && strncasecmp("ABC", "abd", 2) == 0);
    CHECK(strncpy(b, "xy", 5) == b && b[1] == 'y' && b[2] == 0 && b[4] == 0);
    CHECK(strspn("aab", "a") == 2 && strcspn("aab", "b") == 2 && strstr("abcabc", "ca") != NULL);
    CHECK(sprintf(b, "%d-%u", -1, 2u) == 4 && strcmp(b, "-1-2") == 0 && atoi(" 31") == 31);
    CHECK(isalnum('a') && isalpha('a') && isdigit('1') && isgraph('!') && isprint(' ') && isspace('\t'));
    CHECK(isupper('A') && isxdigit('f') && tolower('A') == 'a' && toupper('z') == 'Z' && !isdigit('x'));
    /* Not a character: no class, given back as it is (C leaves it undefined;
     * the device does not index a table with it). */
    volatile int far = 0x7fffff00, neg = -5;
    CHECK(!isdigit(far) && !isalpha(neg) && tolower(neg) == neg && toupper(far) == far);
    /* A string 3 bytes from the end of memory, with an n past it: C's
     * strncmp and strncpy stop at its end. */
    char *end = (char *)&__heap_base - 3;
    end[0] = 'o', end[1] = 'n', end[2] = 0;
    CHECK(strncmp(end, "on", 64) == 0 && strncpy(b, end, sizeof(b)) == b && strcmp(b, "on") == 0);
    return bad;
}

int32_t init(void)
{
    struct big copy = s_big;
    char zero[300] = {0};
    char *end = NULL;
    long v = strtol("  -42xyz", &end, 10);
    const char *volatile hex = "ff"; /* not a constant: clang would fold the call */
    unsigned long h = strtoul(hex, NULL, 16);
    char out[96];
    int n = snprintf(out, sizeof(out), "%s %d %ld %lu %x %.2f %s %d %c bad=%x", copy.text, copy.n, v, h, 255, 3.14159,
                     end, (int)strlen(zero), toupper('a'), others());
    fc_set_state(out, n);
    printf("hello\n");
    printf("n=%d", 5);
    printf(" more\n");
    putchar('o');
    putchar('k');
    putchar('\n');
    puts("line");
    printf("partial");
    return 0;
}

void tick(void) {}
