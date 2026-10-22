/* Parses the --cpus / --control-cpu lists produced by topology.py. CPU numbers
 * come only from the command line; nothing here knows the host layout.
 */
#define _GNU_SOURCE
#include "afcore_internal.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_cpu(const char** s, long* out)
{
    if (**s < '0' || **s > '9')
        return 0;
    char* end;
    errno = 0;
    long v = strtol(*s, &end, 10);
    if (errno || v < 0 || v >= CPU_SETSIZE)
        return 0;
    *s = end;
    *out = v;
    return 1;
}

/* Accepts "a", "a-b" and comma-separated combinations; order is preserved,
 * ranges expand ascending, and duplicates are rejected.
 */
int afc_parse_cpu_list(const char* text, int* out, uint32_t cap, uint32_t* n, char* err, size_t errcap)
{
    const char* s = text;
    *n = 0;
    if (!*s) {
        snprintf(err, errcap, "empty CPU list");
        return AF_ERROR;
    }
    for (;;) {
        long lo, hi;
        if (!parse_cpu(&s, &lo)) {
            snprintf(err, errcap, "bad CPU list '%s'", text);
            return AF_ERROR;
        }
        hi = lo;
        if (*s == '-') {
            s++;
            if (!parse_cpu(&s, &hi) || hi < lo) {
                snprintf(err, errcap, "bad CPU range in '%s'", text);
                return AF_ERROR;
            }
        }
        for (long c = lo; c <= hi; c++) {
            for (uint32_t k = 0; k < *n; k++)
                if (out[k] == c) {
                    snprintf(err, errcap, "duplicate CPU %ld in '%s'", c, text);
                    return AF_ERROR;
                }
            if (*n == cap) {
                snprintf(err, errcap, "more than %u CPUs in '%s'", cap, text);
                return AF_ERROR;
            }
            out[(*n)++] = (int)c;
        }
        if (!*s)
            return AF_OK;
        if (*s != ',') {
            snprintf(err, errcap, "bad CPU list '%s'", text);
            return AF_ERROR;
        }
        s++;
    }
}
