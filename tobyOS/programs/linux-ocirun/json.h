/* json.h -- a small, strict JSON reader for linux-ocirun.
 *
 * Strict on purpose. A container runtime that MISREADS its config is worse than
 * one that refuses it: every field here either grants or withholds isolation, so
 * a tolerant parser that shrugs at a malformed `namespaces` array would hand the
 * caller a container with fewer boundaries than the config asked for, silently.
 * Every function below fails loudly instead of guessing.
 *
 * Deliberately NOT a general-purpose library:
 *   - the node pool is fixed (JSON_MAX_NODES); overflow is an error, not a
 *     silent truncation, because a truncated config is a weakened container
 *   - numbers are parsed as long long via strtoll; JSON's float syntax is
 *     accepted by the tokenizer but the accessors only ever ask for integers,
 *     which is all the OCI fields this runtime honours actually are
 *   - it parses a mutable buffer IN PLACE (strings are unescaped over the top
 *     of the original text), so the caller must keep the buffer alive
 */
#ifndef OCIRUN_JSON_H
#define OCIRUN_JSON_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define JSON_MAX_NODES 4096

enum json_type { J_NULL = 0, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct json_node {
    int   type;
    long long num;      /* J_NUM (integers; see header note)   */
    int   bval;         /* J_BOOL                              */
    char *str;          /* J_STR value, NUL-terminated in place */
    char *key;          /* member name when inside an object    */
    int   first;        /* first child, or -1                   */
    int   next;         /* next sibling, or -1                  */
};

struct json {
    struct json_node n[JSON_MAX_NODES];
    int   count;
    char *p;            /* cursor                              */
    char  err[160];
};

static int json_fail(struct json *j, const char *what)
{
    if (!j->err[0]) {
        /* Report the offset: "unexpected character" without a position is the
         * kind of diagnostic that costs a boot to act on. */
        snprintf(j->err, sizeof j->err, "%s", what);
    }
    return -1;
}

static int json_alloc(struct json *j, int type)
{
    if (j->count >= JSON_MAX_NODES) {
        json_fail(j, "config too large (node pool exhausted) -- refusing, "
                     "because a truncated config is a weakened container");
        return -1;
    }
    int i = j->count++;
    struct json_node *n = &j->n[i];
    n->type = type; n->num = 0; n->bval = 0;
    n->str = NULL; n->key = NULL; n->first = -1; n->next = -1;
    return i;
}

static void json_ws(struct json *j)
{
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')
        j->p++;
}

/* Unescape in place. Returns the start of the NUL-terminated value, or NULL. */
static char *json_string(struct json *j)
{
    if (*j->p != '"') { json_fail(j, "expected a string"); return NULL; }
    j->p++;
    char *out = j->p;          /* writes trail reads, so in-place is safe */
    char *w = out;
    while (*j->p && *j->p != '"') {
        if (*j->p != '\\') { *w++ = *j->p++; continue; }
        j->p++;
        switch (*j->p) {
        case '"':  *w++ = '"';  j->p++; break;
        case '\\': *w++ = '\\'; j->p++; break;
        case '/':  *w++ = '/';  j->p++; break;
        case 'b':  *w++ = '\b'; j->p++; break;
        case 'f':  *w++ = '\f'; j->p++; break;
        case 'n':  *w++ = '\n'; j->p++; break;
        case 'r':  *w++ = '\r'; j->p++; break;
        case 't':  *w++ = '\t'; j->p++; break;
        case 'u': {
            j->p++;
            unsigned cp = 0;
            for (int k = 0; k < 4; k++) {
                char c = *j->p++;
                unsigned d;
                if      (c >= '0' && c <= '9') d = (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
                else { json_fail(j, "bad \\u escape"); return NULL; }
                cp = (cp << 4) | d;
            }
            /* BMP only, encoded as UTF-8. Surrogate pairs are refused rather
             * than mangled -- no OCI field this runtime reads needs them. */
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                json_fail(j, "\\u surrogate pairs are not supported");
                return NULL;
            }
            if (cp < 0x80) *w++ = (char)cp;
            else if (cp < 0x800) {
                *w++ = (char)(0xC0 | (cp >> 6));
                *w++ = (char)(0x80 | (cp & 0x3F));
            } else {
                *w++ = (char)(0xE0 | (cp >> 12));
                *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *w++ = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: json_fail(j, "bad escape sequence"); return NULL;
        }
    }
    if (*j->p != '"') { json_fail(j, "unterminated string"); return NULL; }
    j->p++;                    /* past the closing quote */
    *w = '\0';                 /* safe: w <= the quote we just consumed */
    return out;
}

static int json_value(struct json *j);

static int json_object(struct json *j)
{
    int me = json_alloc(j, J_OBJ);
    if (me < 0) return -1;
    j->p++;                    /* '{' */
    json_ws(j);
    int last = -1;
    if (*j->p == '}') { j->p++; return me; }
    for (;;) {
        json_ws(j);
        char *k = json_string(j);
        if (!k) return -1;
        json_ws(j);
        if (*j->p != ':') return json_fail(j, "expected ':' after a key");
        j->p++;
        int v = json_value(j);
        if (v < 0) return -1;
        j->n[v].key = k;
        if (last < 0) j->n[me].first = v; else j->n[last].next = v;
        last = v;
        json_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return me; }
        return json_fail(j, "expected ',' or '}' in an object");
    }
}

static int json_array(struct json *j)
{
    int me = json_alloc(j, J_ARR);
    if (me < 0) return -1;
    j->p++;                    /* '[' */
    json_ws(j);
    int last = -1;
    if (*j->p == ']') { j->p++; return me; }
    for (;;) {
        int v = json_value(j);
        if (v < 0) return -1;
        if (last < 0) j->n[me].first = v; else j->n[last].next = v;
        last = v;
        json_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; return me; }
        return json_fail(j, "expected ',' or ']' in an array");
    }
}

static int json_value(struct json *j)
{
    json_ws(j);
    switch (*j->p) {
    case '{': return json_object(j);
    case '[': return json_array(j);
    case '"': {
        int me = json_alloc(j, J_STR);
        if (me < 0) return -1;
        char *s = json_string(j);
        if (!s) return -1;
        j->n[me].str = s;
        return me;
    }
    case 't':
        if (strncmp(j->p, "true", 4) != 0) return json_fail(j, "bad literal");
        j->p += 4;
        { int me = json_alloc(j, J_BOOL); if (me < 0) return -1;
          j->n[me].bval = 1; return me; }
    case 'f':
        if (strncmp(j->p, "false", 5) != 0) return json_fail(j, "bad literal");
        j->p += 5;
        { int me = json_alloc(j, J_BOOL); if (me < 0) return -1;
          j->n[me].bval = 0; return me; }
    case 'n':
        if (strncmp(j->p, "null", 4) != 0) return json_fail(j, "bad literal");
        j->p += 4;
        return json_alloc(j, J_NULL);
    default: {
        char *end = NULL;
        long long v = strtoll(j->p, &end, 10);
        if (end == j->p) return json_fail(j, "expected a JSON value");
        /* Reject a fractional/exponent form rather than silently truncating:
         * every numeric OCI field this runtime reads (uid, limits, sizes) is
         * an integer, and a quietly-floored limit is a wrong limit. */
        if (*end == '.' || *end == 'e' || *end == 'E')
            return json_fail(j, "non-integer number (this runtime reads only "
                                "integer fields, and will not round one)");
        j->p = end;
        int me = json_alloc(j, J_NUM);
        if (me < 0) return -1;
        j->n[me].num = v;
        return me;
    }
    }
}

/* Parse `buf` (modified in place). Returns the root node index, or -1 with
 * j->err set. */
static int json_parse(struct json *j, char *buf)
{
    j->count = 0; j->p = buf; j->err[0] = '\0';
    int root = json_value(j);
    if (root < 0) return -1;
    json_ws(j);
    if (*j->p != '\0') return json_fail(j, "trailing garbage after the "
                                           "top-level value");
    return root;
}

/* ---- accessors ---------------------------------------------------------- */

static int json_member(struct json *j, int obj, const char *key)
{
    if (obj < 0 || j->n[obj].type != J_OBJ) return -1;
    for (int c = j->n[obj].first; c >= 0; c = j->n[c].next)
        if (j->n[c].key && strcmp(j->n[c].key, key) == 0) return c;
    return -1;
}

/* "linux.resources.pids.limit" -- returns -1 if any step is missing. */
static int json_path(struct json *j, int root, const char *path)
{
    char tmp[192];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    int cur = root;
    char *s = tmp;
    while (s && *s) {
        char *dot = strchr(s, '.');
        if (dot) *dot = '\0';
        cur = json_member(j, cur, s);
        if (cur < 0) return -1;
        s = dot ? dot + 1 : NULL;
    }
    return cur;
}

static int json_len(struct json *j, int arr)
{
    if (arr < 0 || j->n[arr].type != J_ARR) return 0;
    int k = 0;
    for (int c = j->n[arr].first; c >= 0; c = j->n[c].next) k++;
    return k;
}

static int json_at(struct json *j, int arr, int idx)
{
    if (arr < 0 || j->n[arr].type != J_ARR) return -1;
    for (int c = j->n[arr].first; c >= 0; c = j->n[c].next)
        if (idx-- == 0) return c;
    return -1;
}

static const char *json_str(struct json *j, int node, const char *dflt)
{
    if (node < 0 || j->n[node].type != J_STR) return dflt;
    return j->n[node].str;
}

static long long json_num(struct json *j, int node, long long dflt)
{
    if (node < 0 || j->n[node].type != J_NUM) return dflt;
    return j->n[node].num;
}

#endif /* OCIRUN_JSON_H */
