/* ---------------------------------------------------------------------------
 *  rjson.c -- minimal JSON reader.  See rjson.h for scope and rationale
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  rjson -- minimal JSON reader. See rjson.h for scope and rationale.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-16  (Renesis v92.2)
 *  Created:     Renesis v89.11 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* rjson -- minimal JSON reader.  See rjson.h for scope and rationale. */
#include "rjson.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *err;
} RJ;

static RJValue *parse_value(RJ *s);

static void skip_ws(RJ *s)
{
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')
        s->p++;
}

static RJValue *new_val(RJType t)
{
    RJValue *v = (RJValue *)calloc(1, sizeof(RJValue));
    if (v) v->type = t;
    return v;
}

void rj_free(RJValue *v)
{
    size_t i;
    if (!v) return;
    free(v->str);
    for (i = 0; i < v->n; i++) {
        if (v->keys) free(v->keys[i]);
        if (v->vals) rj_free(v->vals[i]);
    }
    free(v->keys);
    free(v->vals);
    free(v);
}

/* Append one member/element, growing geometrically. */
static int push(RJValue *v, char *key, RJValue *val)
{
    size_t cap = 4;
    while (cap < v->n + 1) cap <<= 1;
    if ((v->n & (v->n - 1)) == 0 || v->n < 4) {   /* grow at powers of two */
        void *nk = v->keys ? realloc(v->keys, cap * sizeof(char *))
                           : (key ? calloc(cap, sizeof(char *)) : NULL);
        void *nv = realloc(v->vals, cap * sizeof(RJValue *));
        if (!nv || (key && !nk)) { free(nk); return 0; }
        if (key || v->keys) v->keys = (char **)nk;
        v->vals = (RJValue **)nv;
    }
    if (v->keys) v->keys[v->n] = key;
    v->vals[v->n] = val;
    v->n++;
    return 1;
}

static int hex4(const char *p, unsigned *out)
{
    unsigned u = 0; int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        u <<= 4;
        if (c >= '0' && c <= '9') u |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') u |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') u |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = u;
    return 1;
}

static char *parse_string_raw(RJ *s)
{
    const char *p = s->p;
    size_t cap, len = 0;
    char *out;
    if (*p != '"') { s->err = "expected string"; return NULL; }
    p++;
    cap = strlen(p) + 1;                 /* upper bound: escapes only shrink */
    out = (char *)malloc(cap);
    if (!out) { s->err = "out of memory"; return NULL; }
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  out[len++] = '"';  p++; break;
            case '\\': out[len++] = '\\'; p++; break;
            case '/':  out[len++] = '/';  p++; break;
            case 'b':  out[len++] = '\b'; p++; break;
            case 'f':  out[len++] = '\f'; p++; break;
            case 'n':  out[len++] = '\n'; p++; break;
            case 'r':  out[len++] = '\r'; p++; break;
            case 't':  out[len++] = '\t'; p++; break;
            case 'u': {
                unsigned u;
                if (!hex4(p + 1, &u)) {
                    s->err = "bad \\u escape"; free(out); return NULL;
                }
                p += 5;
                if (u < 0x80) out[len++] = (char)u;
                else if (u < 0x800) {
                    out[len++] = (char)(0xC0 | (u >> 6));
                    out[len++] = (char)(0x80 | (u & 0x3F));
                } else {
                    out[len++] = (char)(0xE0 | (u >> 12));
                    out[len++] = (char)(0x80 | ((u >> 6) & 0x3F));
                    out[len++] = (char)(0x80 | (u & 0x3F));
                }
                break;
            }
            default:
                s->err = "bad escape"; free(out); return NULL;
            }
        } else {
            out[len++] = *p++;
        }
    }
    if (*p != '"') { s->err = "unterminated string"; free(out); return NULL; }
    out[len] = '\0';
    s->p = p + 1;
    return out;
}

static RJValue *parse_object(RJ *s)
{
    RJValue *v = new_val(RJ_OBJ);
    if (!v) { s->err = "out of memory"; return NULL; }
    s->p++;                                   /* '{' */
    skip_ws(s);
    if (*s->p == '}') { s->p++; return v; }
    for (;;) {
        char *key;
        RJValue *val;
        skip_ws(s);
        key = parse_string_raw(s);
        if (!key) { rj_free(v); return NULL; }
        skip_ws(s);
        if (*s->p != ':') {
            s->err = "expected ':'"; free(key); rj_free(v); return NULL;
        }
        s->p++;
        val = parse_value(s);
        if (!val) { free(key); rj_free(v); return NULL; }
        if (!push(v, key, val)) {
            s->err = "out of memory";
            free(key); rj_free(val); rj_free(v); return NULL;
        }
        skip_ws(s);
        if (*s->p == ',') { s->p++; continue; }
        if (*s->p == '}') { s->p++; return v; }
        s->err = "expected ',' or '}'";
        rj_free(v);
        return NULL;
    }
}

static RJValue *parse_array(RJ *s)
{
    RJValue *v = new_val(RJ_ARR);
    if (!v) { s->err = "out of memory"; return NULL; }
    s->p++;                                   /* '[' */
    skip_ws(s);
    if (*s->p == ']') { s->p++; return v; }
    for (;;) {
        RJValue *val = parse_value(s);
        if (!val) { rj_free(v); return NULL; }
        if (!push(v, NULL, val)) {
            s->err = "out of memory"; rj_free(val); rj_free(v); return NULL;
        }
        skip_ws(s);
        if (*s->p == ',') { s->p++; continue; }
        if (*s->p == ']') { s->p++; return v; }
        s->err = "expected ',' or ']'";
        rj_free(v);
        return NULL;
    }
}

static RJValue *parse_value(RJ *s)
{
    RJValue *v;
    skip_ws(s);
    switch (*s->p) {
    case '{': return parse_object(s);
    case '[': return parse_array(s);
    case '"':
        v = new_val(RJ_STR);
        if (!v) { s->err = "out of memory"; return NULL; }
        v->str = parse_string_raw(s);
        if (!v->str) { rj_free(v); return NULL; }
        return v;
    case 't':
        if (strncmp(s->p, "true", 4)) { s->err = "bad literal"; return NULL; }
        s->p += 4;
        v = new_val(RJ_BOOL); if (v) v->b = 1;
        return v;
    case 'f':
        if (strncmp(s->p, "false", 5)) { s->err = "bad literal"; return NULL; }
        s->p += 5;
        v = new_val(RJ_BOOL); if (v) v->b = 0;
        return v;
    case 'n':
        if (strncmp(s->p, "null", 4)) { s->err = "bad literal"; return NULL; }
        s->p += 4;
        return new_val(RJ_NULL);
    default: {
        char *end = NULL;
        double d = strtod(s->p, &end);
        if (end == s->p) { s->err = "expected a value"; return NULL; }
        s->p = end;
        v = new_val(RJ_NUM);
        if (v) v->num = d;
        return v;
    }
    }
}

RJValue *rj_parse(const char *text, const char **err)
{
    RJ s;
    RJValue *v;
    s.p = text;
    s.err = NULL;
    v = parse_value(&s);
    if (!v) { if (err) *err = s.err ? s.err : "parse error"; return NULL; }
    skip_ws(&s);
    if (*s.p) {
        if (err) *err = "trailing content after the document";
        rj_free(v);
        return NULL;
    }
    return v;
}

RJValue *rj_parse_file(const char *path, const char **err)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    RJValue *v;
    if (!f) { if (err) *err = "cannot open file"; return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (err) *err = "cannot seek";
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) { fclose(f); if (err) *err = "cannot size"; return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); if (err) *err = "out of memory"; return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); if (err) *err = "short read"; return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    v = rj_parse(buf, err);
    free(buf);
    return v;
}

const RJValue *rj_get(const RJValue *v, const char *key)
{
    size_t i;
    if (!v || v->type != RJ_OBJ || !v->keys) return NULL;
    for (i = 0; i < v->n; i++)
        if (v->keys[i] && !strcmp(v->keys[i], key)) return v->vals[i];
    return NULL;
}

const RJValue *rj_path(const RJValue *v, ...)
{
    va_list ap;
    const char *k;
    va_start(ap, v);
    while ((k = va_arg(ap, const char *)) != NULL) {
        v = rj_get(v, k);
        if (!v) break;
    }
    va_end(ap);
    return v;
}

double rj_num(const RJValue *v, const char *key, double dflt)
{
    const RJValue *m = rj_get(v, key);
    return (m && m->type == RJ_NUM) ? m->num : dflt;
}

int rj_bool(const RJValue *v, const char *key, int dflt)
{
    const RJValue *m = rj_get(v, key);
    if (!m) return dflt;
    if (m->type == RJ_BOOL) return m->b;
    if (m->type == RJ_NUM)  return m->num != 0.0;
    return dflt;
}

const char *rj_str(const RJValue *v, const char *key, const char *dflt)
{
    const RJValue *m = rj_get(v, key);
    return (m && m->type == RJ_STR) ? m->str : dflt;
}
