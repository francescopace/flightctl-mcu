/* The server sends the entire Device, including the echoed status and an
 * encoded copy of all its console sessions. A C3 may have more than enough
 * free RAM for it but no single free block as large as the response. */
#include "fc_rendered.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct fc_rendered_page {
    struct fc_rendered_page *next;
    size_t len;
    char data[1024];
};

int fc_rendered_feed(const void *data, size_t len, void *ctx)
{
    fc_rendered_body_t *b = ctx;
    const char *src = data;
    while (len) {
        fc_rendered_page_t *p = b->last;
        if (!p || p->len == sizeof(p->data)) {
            p = malloc(sizeof(*p));
            if (!p) {
                return -1;
            }
            p->next = NULL;
            p->len = 0;
            if (b->last) {
                b->last->next = p;
            } else {
                b->first = p;
            }
            b->last = p;
        }
        size_t n = sizeof(p->data) - p->len;
        if (n > len) {
            n = len;
        }
        memcpy(p->data + p->len, src, n);
        p->len += n;
        b->size += n;
        src += n;
        len -= n;
    }
    return 0;
}

void fc_rendered_free(fc_rendered_body_t *b)
{
    while (b->first) {
        fc_rendered_page_t *next = b->first->next;
        free(b->first);
        b->first = next;
    }
    *b = (fc_rendered_body_t){0};
}

typedef struct {
    const fc_rendered_page_t *page;
    size_t i, pos;
} cursor_t;

static int peek(cursor_t p)
{
    return p.page ? (unsigned char)p.page->data[p.i] : -1;
}

static int take(cursor_t *p)
{
    int c = peek(*p);
    if (p->page) {
        p->pos++;
        if (++p->i == p->page->len) {
            p->page = p->page->next;
            p->i = 0;
        }
    }
    return c;
}

static void space(cursor_t *p)
{
    int c;
    while ((c = peek(*p)) == ' ' || c == '\t' || c == '\r' || c == '\n') {
        take(p);
    }
}

static int hex4(cursor_t *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = take(p);
        if (!isxdigit(c)) {
            return -1;
        }
        v = 16 * v + (isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
    }
    return v;
}

/* Match the decoded key just as cJSON_GetObjectItem does. All the keys
 * sought here are ASCII. Validate escapes even in fields skipped. */
static int string(cursor_t *p, const char *key, int *match)
{
    if (take(p) != '"') {
        return 0;
    }
    int c;
    while ((c = take(p)) >= ' ' && c != '"') {
        if (c == '\\') {
            c = take(p);
            if (c == 'u') {
                c = hex4(p);
                if (c < 0 || (c >= 0xdc00 && c <= 0xdfff)) {
                    return 0;
                }
                if (c >= 0xd800 && c < 0xdc00) {
                    if (take(p) != '\\' || take(p) != 'u') {
                        return 0;
                    }
                    int low = hex4(p);
                    if (low < 0xdc00 || low > 0xdfff) {
                        return 0;
                    }
                }
            } else if (c <= 0 || !strchr("\"\\/bfnrt", c)) {
                return 0;
            } else if (strchr("bfnrt", c)) {
                c = 0; /* a control character never matches a field name */
            }
        }
        if (key && (!*key || c > 127 || tolower(c) != tolower(*key++))) {
            key = NULL;
        }
    }
    if (match) {
        *match = key && !*key;
    }
    return c == '"';
}

static int digits(cursor_t *p)
{
    if (!isdigit(peek(*p))) {
        return 0;
    }
    do {
        take(p);
    } while (isdigit(peek(*p)));
    return 1;
}

static int number(cursor_t *p)
{
    if (peek(*p) == '-') {
        take(p);
    }
    if (peek(*p) == '0') {
        take(p);
    } else if (!digits(p)) {
        return 0;
    }
    if (peek(*p) == '.') {
        take(p);
        if (!digits(p)) {
            return 0;
        }
    }
    if (peek(*p) == 'e' || peek(*p) == 'E') {
        take(p);
        if (peek(*p) == '+' || peek(*p) == '-') {
            take(p);
        }
        return digits(p);
    }
    return 1;
}

/* The stack holds one closing delimiter per open object or array, even
 * for a value the agent ignores. Every key, separator and value of a
 * changed device must still be valid before it can change the firmware. */
static int skip(cursor_t *p)
{
    char ends[CJSON_NESTING_LIMIT];
    size_t depth = 0;
    for (;;) {
        int c = peek(*p);
        if (c == '{' || c == '[') {
            if (depth == sizeof(ends)) {
                return 0;
            }
            take(p);
            space(p);
            int end = c == '{' ? '}' : ']';
            if (peek(*p) != end) {
                ends[depth++] = end;
                goto next_member;
            }
            take(p);
        } else if (c == '"') {
            if (!string(p, NULL, NULL)) {
                return 0;
            }
        } else if (c == 't' || c == 'f' || c == 'n') {
            const char *literal = c == 't' ? "true" : c == 'f' ? "false" : "null";
            while (*literal) {
                if (take(p) != *literal++) {
                    return 0;
                }
            }
        } else if (!number(p)) {
            return 0;
        }
        while (depth) {
            space(p);
            c = take(p);
            if (c == ',') {
                space(p);
                break;
            }
            if (c != ends[--depth]) {
                return 0;
            }
        }
        if (!depth) {
            return 1;
        }
next_member:
        if (ends[depth - 1] == '}') {
            if (!string(p, NULL, NULL)) {
                return 0;
            }
            space(p);
            if (take(p) != ':') {
                return 0;
            }
            space(p);
        }
    }
}

typedef struct {
    cursor_t begin, end;
} span_t;

static span_t member(cursor_t p, const char *key)
{
    space(&p);
    if (take(&p) == '{') {
        space(&p);
        while (peek(p) == '"') {
            int match;
            if (!string(&p, key, &match)) {
                break;
            }
            space(&p);
            if (take(&p) != ':') {
                break;
            }
            space(&p);
            cursor_t start = p;
            if (!skip(&p)) {
                break;
            }
            if (match) {
                return (span_t){start, p};
            }
            space(&p);
            if (take(&p) != ',') {
                break;
            }
            space(&p);
        }
    }
    return (span_t){0};
}

static char *copy(char *dst, cursor_t p, size_t n)
{
    while (n) {
        size_t step = p.page->len - p.i;
        if (step > n) {
            step = n;
        }
        memcpy(dst, p.page->data + p.i, step);
        dst += step;
        n -= step;
        p.page = p.page->next;
        p.i = 0;
    }
    return dst;
}

/* Replace the console array by null in the spec read for reconciliation.
 * Its entries have already been parsed; the rest of the spec, including
 * unknown members and the placement of commas, is preserved verbatim. */
static cJSON *parse(span_t value, span_t omit)
{
    if (!value.begin.page) {
        return NULL;
    }
    size_t n = value.end.pos - value.begin.pos;
    if (omit.begin.page) {
        n = n - (omit.end.pos - omit.begin.pos) + 4;
    }
    char *text = malloc(n + 1);
    if (!text) {
        return NULL;
    }
    char *dst = text;
    if (omit.begin.page) {
        dst = copy(dst, value.begin, omit.begin.pos - value.begin.pos);
        memcpy(dst, "null", 4);
        dst += 4;
        value.begin = omit.end;
    }
    *copy(dst, value.begin, value.end.pos - value.begin.pos) = '\0';
    cJSON *json = cJSON_ParseWithLengthOpts(text, n + 1, NULL, 1);
    free(text);
    return json;
}

void fc_rendered_clear(fc_rendered_t *r)
{
    cJSON_Delete(r->version);
    cJSON_Delete(r->consoles);
    cJSON_Delete(r->spec);
    *r = (fc_rendered_t){0};
}

int fc_rendered_read(fc_rendered_body_t *body, const char *known, int refresh, fc_rendered_t *r)
{
    *r = (fc_rendered_t){0};
    cursor_t start = {.page = body->first};
    space(&start);
    cursor_t end = start;
    if (peek(start) != '{' || !skip(&end)) {
        goto fail;
    }
    space(&end);
    if (end.page) {
        goto fail;
    }
    span_t meta = member(start, "metadata");
    span_t ann = member(meta.begin, "annotations");
    span_t version = member(ann.begin, RENDERED_ANNOTATION);
    span_t spec = member(start, "spec");
    span_t list = member(spec.begin, "consoles");
    if (version.begin.page && !(r->version = parse(version, (span_t){0}))) {
        goto fail;
    }
    if (peek(list.begin) == '[' && !(r->consoles = parse(list, (span_t){0}))) {
        goto fail;
    }
    const char *v = cJSON_GetStringValue(r->version);
    r->same = known && *known && v && strcmp(v, known) == 0;
    if ((!r->same || (!refresh && !cJSON_GetArraySize(r->consoles))) && spec.begin.page) {
        r->spec = parse(spec, r->consoles ? list : (span_t){0});
        if (!r->spec) {
            goto fail;
        }
    }
    fc_rendered_free(body);
    return 0;

fail:
    fc_rendered_free(body);
    fc_rendered_clear(r);
    return -1;
}
