/* Minimal JSON reader/writer for CLI inputs (params, knobs, meta, targets,
 * telemetry) and the JSONL record writer.
 */
#define _GNU_SOURCE
#include "afcore_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AFC_JSON_MAX_DEPTH 64

void afc_buf_put(AfcBuf* b, const char* s, size_t n)
{
    if (b->oom)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap)
            cap *= 2;
        char* p = realloc(b->p, cap);
        if (!p) {
            b->oom = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void afc_buf_puts(AfcBuf* b, const char* s) { afc_buf_put(b, s, strlen(s)); }

void afc_buf_printf(AfcBuf* b, const char* fmt, ...)
{
    char small[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->oom = 1;
        return;
    }
    if ((size_t)n < sizeof small) {
        afc_buf_put(b, small, (size_t)n);
        return;
    }
    char* big = malloc((size_t)n + 1);
    if (!big) {
        b->oom = 1;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    afc_buf_put(b, big, (size_t)n);
    free(big);
}

void afc_buf_json_string(AfcBuf* b, const char* s, size_t n)
{
    afc_buf_put(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': afc_buf_put(b, "\\\"", 2); break;
        case '\\': afc_buf_put(b, "\\\\", 2); break;
        case '\n': afc_buf_put(b, "\\n", 2); break;
        case '\r': afc_buf_put(b, "\\r", 2); break;
        case '\t': afc_buf_put(b, "\\t", 2); break;
        default:
            if (c < 0x20)
                afc_buf_printf(b, "\\u%04x", c);
            else
                afc_buf_put(b, (const char*)&s[i], 1);
        }
    }
    afc_buf_put(b, "\"", 1);
}

void afc_buf_free(AfcBuf* b)
{
    free(b->p);
    *b = (AfcBuf){0};
}

typedef struct Parser {
    const char* s;
    size_t n, i;
    char* err;
    size_t cap;
    int failed;
} Parser;

static void perr(Parser* p, const char* what)
{
    if (!p->failed)
        snprintf(p->err, p->cap, "JSON: %s at byte %zu", what, p->i);
    p->failed = 1;
}

static void skip_ws(Parser* p)
{
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' ||
                           p->s[p->i] == '\r'))
        p->i++;
}

static int literal(Parser* p, const char* word)
{
    size_t k = strlen(word);
    if (p->n - p->i < k || memcmp(p->s + p->i, word, k)) {
        perr(p, "invalid literal");
        return 0;
    }
    p->i += k;
    return 1;
}

static int hex4(Parser* p, unsigned* out)
{
    if (p->n - p->i < 4) {
        perr(p, "short \\u escape");
        return 0;
    }
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p->s[p->i++];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else {
            perr(p, "bad \\u escape");
            return 0;
        }
    }
    *out = v;
    return 1;
}

static void put_utf8(AfcBuf* b, unsigned cp)
{
    char u[4];
    size_t k;
    if (cp < 0x80) {
        u[0] = (char)cp;
        k = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        k = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        k = 3;
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        k = 4;
    }
    afc_buf_put(b, u, k);
}

static int parse_string(Parser* p, char** out, size_t* len)
{
    AfcBuf b = {0};
    p->i++; /* opening quote */
    for (;;) {
        if (p->i >= p->n) {
            perr(p, "unterminated string");
            break;
        }
        unsigned char c = (unsigned char)p->s[p->i++];
        if (c == '"') {
            afc_buf_put(&b, "", 0);
            if (b.oom) {
                perr(p, "out of memory");
                break;
            }
            *out = b.p ? b.p : calloc(1, 1);
            *len = b.len;
            return *out != NULL;
        }
        if (c < 0x20) {
            perr(p, "control character in string");
            break;
        }
        if (c != '\\') {
            afc_buf_put(&b, (const char*)&c, 1);
            continue;
        }
        if (p->i >= p->n) {
            perr(p, "unterminated escape");
            break;
        }
        char e = p->s[p->i++];
        unsigned cp;
        switch (e) {
        case '"': afc_buf_put(&b, "\"", 1); break;
        case '\\': afc_buf_put(&b, "\\", 1); break;
        case '/': afc_buf_put(&b, "/", 1); break;
        case 'b': afc_buf_put(&b, "\b", 1); break;
        case 'f': afc_buf_put(&b, "\f", 1); break;
        case 'n': afc_buf_put(&b, "\n", 1); break;
        case 'r': afc_buf_put(&b, "\r", 1); break;
        case 't': afc_buf_put(&b, "\t", 1); break;
        case 'u':
            if (!hex4(p, &cp))
                goto fail;
            if (cp >= 0xD800 && cp < 0xDC00) {
                unsigned lo;
                if (p->n - p->i < 2 || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u') {
                    perr(p, "unpaired surrogate");
                    goto fail;
                }
                p->i += 2;
                if (!hex4(p, &lo))
                    goto fail;
                if (lo < 0xDC00 || lo >= 0xE000) {
                    perr(p, "unpaired surrogate");
                    goto fail;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp < 0xE000) {
                perr(p, "unpaired surrogate");
                goto fail;
            }
            put_utf8(&b, cp);
            break;
        default: perr(p, "bad escape"); goto fail;
        }
    }
fail:
    afc_buf_free(&b);
    return 0;
}

static int parse_number(Parser* p, AfcJson* v)
{
    size_t start = p->i;
    if (p->i < p->n && p->s[p->i] == '-')
        p->i++;
    if (p->i >= p->n) {
        perr(p, "bad number");
        return 0;
    }
    if (p->s[p->i] == '0')
        p->i++;
    else if (p->s[p->i] >= '1' && p->s[p->i] <= '9')
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
            p->i++;
    else {
        perr(p, "bad number");
        return 0;
    }
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        size_t d = p->i;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
            p->i++;
        if (d == p->i) {
            perr(p, "bad fraction");
            return 0;
        }
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-'))
            p->i++;
        size_t d = p->i;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
            p->i++;
        if (d == p->i) {
            perr(p, "bad exponent");
            return 0;
        }
    }
    v->type = AFC_JNUM;
    v->len = p->i - start;
    v->str = strndup(p->s + start, v->len);
    if (!v->str)
        perr(p, "out of memory");
    return v->str != NULL;
}

static int parse_value(Parser* p, AfcJson* v, int depth);

static int grow(void** ptr, size_t elem, size_t* cap, size_t need)
{
    if (need <= *cap)
        return 1;
    size_t c = *cap ? *cap * 2 : 4;
    while (c < need)
        c *= 2;
    void* q = realloc(*ptr, c * elem);
    if (!q)
        return 0;
    *ptr = q;
    *cap = c;
    return 1;
}

static int parse_container(Parser* p, AfcJson* v, int depth, int object)
{
    char close = object ? '}' : ']';
    size_t cap = 0, kcap = 0, lcap = 0;
    v->type = object ? AFC_JOBJ : AFC_JARR;
    p->i++;
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == close) {
        p->i++;
        return 1;
    }
    for (;;) {
        char* key = NULL;
        size_t key_len = 0;
        if (object) {
            skip_ws(p);
            if (p->i >= p->n || p->s[p->i] != '"') {
                perr(p, "expected member name");
                return 0;
            }
            if (!parse_string(p, &key, &key_len))
                return 0;
            for (size_t k = 0; k < v->len; k++)
                if (v->key_lens[k] == key_len && !memcmp(v->keys[k], key, key_len)) {
                    free(key);
                    perr(p, "duplicate member name");
                    return 0;
                }
            skip_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') {
                free(key);
                perr(p, "expected ':'");
                return 0;
            }
            p->i++;
        }
        if (!grow((void**)&v->items, sizeof *v->items, &cap, v->len + 1) ||
            (object && (!grow((void**)&v->keys, sizeof *v->keys, &kcap, v->len + 1) ||
                        !grow((void**)&v->key_lens, sizeof *v->key_lens, &lcap, v->len + 1)))) {
            free(key);
            perr(p, "out of memory");
            return 0;
        }
        AfcJson* item = &v->items[v->len];
        *item = (AfcJson){0};
        if (object) {
            v->keys[v->len] = key;
            v->key_lens[v->len] = key_len;
        }
        v->len++;
        if (!parse_value(p, item, depth + 1))
            return 0;
        skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (p->i < p->n && p->s[p->i] == close) {
            p->i++;
            return 1;
        }
        perr(p, object ? "expected ',' or '}'" : "expected ',' or ']'");
        return 0;
    }
}

static int parse_value(Parser* p, AfcJson* v, int depth)
{
    if (depth > AFC_JSON_MAX_DEPTH) {
        perr(p, "nesting too deep");
        return 0;
    }
    skip_ws(p);
    if (p->i >= p->n) {
        perr(p, "unexpected end");
        return 0;
    }
    switch (p->s[p->i]) {
    case '{': return parse_container(p, v, depth, 1);
    case '[': return parse_container(p, v, depth, 0);
    case '"':
        v->type = AFC_JSTR;
        return parse_string(p, &v->str, &v->len);
    case 't':
        v->type = AFC_JBOOL;
        v->boolean = 1;
        return literal(p, "true");
    case 'f':
        v->type = AFC_JBOOL;
        return literal(p, "false");
    case 'n':
        v->type = AFC_JNULL;
        return literal(p, "null");
    default: return parse_number(p, v);
    }
}

static void free_inner(AfcJson* v)
{
    for (size_t k = 0; v->items && k < v->len; k++) {
        free_inner(&v->items[k]);
        if (v->keys)
            free(v->keys[k]);
    }
    free(v->items);
    free(v->keys);
    free(v->key_lens);
    free(v->str);
}

void afc_json_free(AfcJson* v)
{
    if (!v)
        return;
    free_inner(v);
    free(v);
}

AfcJson* afc_json_parse(const char* text, size_t n, char* err, size_t cap)
{
    Parser p = {text, n, 0, err, cap, 0};
    AfcJson* v = calloc(1, sizeof *v);
    if (!v) {
        snprintf(err, cap, "JSON: out of memory");
        return NULL;
    }
    if (parse_value(&p, v, 0)) {
        skip_ws(&p);
        if (p.i != p.n)
            perr(&p, "trailing data");
    }
    if (p.failed) {
        afc_json_free(v);
        return NULL;
    }
    return v;
}

const AfcJson* afc_json_get(const AfcJson* obj, const char* key)
{
    if (!obj || obj->type != AFC_JOBJ)
        return NULL;
    size_t k = strlen(key);
    for (size_t i = 0; i < obj->len; i++)
        if (obj->key_lens[i] == k && !memcmp(obj->keys[i], key, k))
            return &obj->items[i];
    return NULL;
}

int afc_json_u64(const AfcJson* v, uint64_t* out)
{
    if (!v || v->type != AFC_JNUM || v->str[0] == '-' || strpbrk(v->str, ".eE"))
        return 0;
    errno = 0;
    char* end;
    unsigned long long x = strtoull(v->str, &end, 10);
    if (errno || *end)
        return 0;
    *out = x;
    return 1;
}

void afc_json_write(AfcBuf* b, const AfcJson* v)
{
    switch (v->type) {
    case AFC_JNULL: afc_buf_puts(b, "null"); break;
    case AFC_JBOOL: afc_buf_puts(b, v->boolean ? "true" : "false"); break;
    case AFC_JNUM: afc_buf_put(b, v->str, v->len); break;
    case AFC_JSTR: afc_buf_json_string(b, v->str, v->len); break;
    case AFC_JARR:
    case AFC_JOBJ:
        afc_buf_puts(b, v->type == AFC_JOBJ ? "{" : "[");
        for (size_t i = 0; i < v->len; i++) {
            if (i)
                afc_buf_puts(b, ",");
            if (v->type == AFC_JOBJ) {
                afc_buf_json_string(b, v->keys[i], v->key_lens[i]);
                afc_buf_puts(b, ":");
            }
            afc_json_write(b, &v->items[i]);
        }
        afc_buf_puts(b, v->type == AFC_JOBJ ? "}" : "]");
        break;
    }
}

char* afc_read_file(const char* path, size_t* n, char* err, size_t cap)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(err, cap, "%s: %s", path, strerror(errno));
        return NULL;
    }
    AfcBuf b = {0};
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
        afc_buf_put(&b, chunk, got);
    int bad = ferror(f);
    fclose(f);
    afc_buf_put(&b, "", 0);
    if (bad || b.oom) {
        snprintf(err, cap, "%s: read failed", path);
        afc_buf_free(&b);
        return NULL;
    }
    *n = b.len;
    return b.p;
}
