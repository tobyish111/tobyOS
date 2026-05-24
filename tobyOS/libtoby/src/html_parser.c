/* libtoby/src/html_parser.c -- simplified HTML5 tokenizer + tree builder. */

#include <toby/html.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

#define MAX_NODES      2048
#define STACK_MAX      256

/* ------------------------------------------------------------------ */
/*  Node allocation                                                    */
/* ------------------------------------------------------------------ */

struct node_pool {
    struct dom_node *nodes;
    int count;
};

static struct dom_node *alloc_node(struct node_pool *pool,
                                   enum dom_node_type type)
{
    if (pool->count >= MAX_NODES)
        return 0;
    struct dom_node *n = &pool->nodes[pool->count++];
    memset(n, 0, sizeof(*n));
    n->type = type;
    return n;
}

static void append_child(struct dom_node *parent, struct dom_node *child)
{
    child->parent = parent;
    child->next_sibling = 0;
    if (!parent->first_child) {
        parent->first_child = child;
    } else {
        struct dom_node *s = parent->first_child;
        while (s->next_sibling) s = s->next_sibling;
        s->next_sibling = child;
    }
}

/* ------------------------------------------------------------------ */
/*  Void-element and auto-close tables                                 */
/* ------------------------------------------------------------------ */

static const char *void_tags[] = {
    "area","base","br","col","embed","hr","img","input",
    "link","meta","param","source","track","wbr", 0
};

static int is_void_tag(const char *tag)
{
    for (int i = 0; void_tags[i]; i++)
        if (strcmp(tag, void_tags[i]) == 0) return 1;
    return 0;
}

static int should_auto_close(const char *open, const char *new_tag)
{
    if (strcmp(open, "p") == 0) {
        static const char *block[] = {
            "address","article","aside","blockquote","details","dialog",
            "dd","div","dl","dt","fieldset","figcaption","figure","footer",
            "form","h1","h2","h3","h4","h5","h6","header","hgroup","hr",
            "li","main","nav","ol","p","pre","section","table","ul", 0
        };
        for (int i = 0; block[i]; i++)
            if (strcmp(new_tag, block[i]) == 0) return 1;
    }
    if (strcmp(open, "li") == 0 && strcmp(new_tag, "li") == 0) return 1;
    if (strcmp(open, "dt") == 0 &&
        (strcmp(new_tag, "dt") == 0 || strcmp(new_tag, "dd") == 0))
        return 1;
    if (strcmp(open, "dd") == 0 &&
        (strcmp(new_tag, "dt") == 0 || strcmp(new_tag, "dd") == 0))
        return 1;
    if (strcmp(open, "td") == 0 &&
        (strcmp(new_tag, "td") == 0 || strcmp(new_tag, "th") == 0))
        return 1;
    if (strcmp(open, "th") == 0 &&
        (strcmp(new_tag, "td") == 0 || strcmp(new_tag, "th") == 0))
        return 1;
    if (strcmp(open, "tr") == 0 && strcmp(new_tag, "tr") == 0) return 1;
    if (strcmp(open, "option") == 0 && strcmp(new_tag, "option") == 0)
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Entity decoding                                                    */
/* ------------------------------------------------------------------ */

static int decode_entity(const char *p, const char *end, char *out, int *eaten)
{
    if (p >= end || *p != '&') return 0;
    const char *start = p;
    p++;
    if (p >= end) return 0;

    if (*p == '#') {
        p++;
        unsigned long cp = 0;
        if (p < end && (*p == 'x' || *p == 'X')) {
            p++;
            while (p < end && isxdigit((unsigned char)*p)) {
                int d = isdigit((unsigned char)*p) ? *p - '0'
                        : 10 + (tolower((unsigned char)*p) - 'a');
                cp = cp * 16 + (unsigned long)d;
                p++;
            }
        } else {
            while (p < end && isdigit((unsigned char)*p)) {
                cp = cp * 10 + (unsigned long)(*p - '0');
                p++;
            }
        }
        if (p < end && *p == ';') p++;
        *eaten = (int)(p - start);
        if (cp < 0x80) {
            out[0] = (char)cp;
            return 1;
        } else if (cp < 0x800) {
            out[0] = (char)(0xC0 | (cp >> 6));
            out[1] = (char)(0x80 | (cp & 0x3F));
            return 2;
        } else if (cp < 0x10000) {
            out[0] = (char)(0xE0 | (cp >> 12));
            out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[2] = (char)(0x80 | (cp & 0x3F));
            return 3;
        }
        out[0] = '?';
        return 1;
    }

    struct { const char *name; const char *val; } named[] = {
        {"amp",  "&"}, {"lt",   "<"}, {"gt",   ">"},
        {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
        {"copy", "\xC2\xA9"}, {"reg",  "\xC2\xAE"},
        {"mdash","\xE2\x80\x94"}, {"ndash","\xE2\x80\x93"},
        {"laquo","\xC2\xAB"}, {"raquo","\xC2\xBB"},
        {0,0}
    };
    for (int i = 0; named[i].name; i++) {
        size_t nlen = strlen(named[i].name);
        if ((size_t)(end - p) >= nlen &&
            strncmp(p, named[i].name, nlen) == 0) {
            const char *after = p + nlen;
            if (after < end && *after == ';') after++;
            *eaten = (int)(after - start);
            size_t vlen = strlen(named[i].val);
            memcpy(out, named[i].val, vlen);
            return (int)vlen;
        }
    }
    out[0] = '&';
    *eaten = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Tokenizer helpers                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *src;
    const char *end;
    const char *pos;
} html_tok_t;

static void skip_ws(html_tok_t *t)
{
    while (t->pos < t->end && isspace((unsigned char)*t->pos))
        t->pos++;
}

static void read_tag_name(html_tok_t *t, char *buf, int max)
{
    int i = 0;
    while (t->pos < t->end && i < max - 1) {
        char c = *t->pos;
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':') {
            buf[i++] = (char)tolower((unsigned char)c);
            t->pos++;
        } else {
            break;
        }
    }
    buf[i] = 0;
}

static void read_attr_name(html_tok_t *t, char *buf, int max)
{
    int i = 0;
    while (t->pos < t->end && i < max - 1) {
        char c = *t->pos;
        if (c == '=' || c == '>' || c == '/' || isspace((unsigned char)c))
            break;
        buf[i++] = (char)tolower((unsigned char)c);
        t->pos++;
    }
    buf[i] = 0;
}

static void read_attr_value(html_tok_t *t, char *buf, int max)
{
    skip_ws(t);
    if (t->pos >= t->end) { buf[0] = 0; return; }

    char quote = 0;
    if (*t->pos == '"' || *t->pos == '\'') {
        quote = *t->pos;
        t->pos++;
    }

    int i = 0;
    while (t->pos < t->end && i < max - 1) {
        if (quote) {
            if (*t->pos == quote) { t->pos++; break; }
        } else {
            if (isspace((unsigned char)*t->pos) || *t->pos == '>')
                break;
        }
        if (*t->pos == '&') {
            char ent[8];
            int eaten = 0;
            int n = decode_entity(t->pos, t->end, ent, &eaten);
            for (int j = 0; j < n && i < max - 1; j++)
                buf[i++] = ent[j];
            t->pos += eaten;
        } else {
            buf[i++] = *t->pos;
            t->pos++;
        }
    }
    buf[i] = 0;
}

/* ------------------------------------------------------------------ */
/*  Main parser                                                        */
/* ------------------------------------------------------------------ */

struct dom_document *html_parse(const char *html, size_t len)
{
    struct dom_document *doc = (struct dom_document *)calloc(1, sizeof(*doc));
    if (!doc) return 0;

    struct dom_node *nodes = (struct dom_node *)calloc(MAX_NODES,
                                                       sizeof(struct dom_node));
    if (!nodes) { free(doc); return 0; }

    struct node_pool pool = { nodes, 0 };

    struct dom_node *doc_node = alloc_node(&pool, DOM_DOCUMENT);
    struct dom_node *html_el = alloc_node(&pool, DOM_ELEMENT);
    strncpy(html_el->tag, "html", sizeof(html_el->tag));
    append_child(doc_node, html_el);
    doc->root = html_el;

    struct dom_node *head_el = alloc_node(&pool, DOM_ELEMENT);
    strncpy(head_el->tag, "head", sizeof(head_el->tag));
    append_child(html_el, head_el);
    doc->head = head_el;

    struct dom_node *body_el = alloc_node(&pool, DOM_ELEMENT);
    strncpy(body_el->tag, "body", sizeof(body_el->tag));
    append_child(html_el, body_el);
    doc->body = body_el;

    struct dom_node *stack[STACK_MAX];
    int stack_top = 0;
    stack[stack_top++] = body_el;

    int in_head = 0;
    int in_title = 0;
    int title_pos = 0;

    html_tok_t tok = { html, html + len, html };

    while (tok.pos < tok.end) {
        if (*tok.pos == '<') {
            tok.pos++;
            if (tok.pos >= tok.end) break;

            /* Comment: <!-- ... --> */
            if (tok.end - tok.pos >= 3 && tok.pos[0] == '!'
                && tok.pos[1] == '-' && tok.pos[2] == '-') {
                tok.pos += 3;
                while (tok.pos < tok.end) {
                    if (tok.end - tok.pos >= 3 && tok.pos[0] == '-'
                        && tok.pos[1] == '-' && tok.pos[2] == '>') {
                        tok.pos += 3;
                        break;
                    }
                    tok.pos++;
                }
                continue;
            }

            /* DOCTYPE */
            if (tok.end - tok.pos >= 1 && *tok.pos == '!') {
                while (tok.pos < tok.end && *tok.pos != '>') tok.pos++;
                if (tok.pos < tok.end) tok.pos++;
                continue;
            }

            /* CDATA: <![CDATA[ ... ]]> */
            if (tok.end - tok.pos >= 8 &&
                strncmp(tok.pos, "![CDATA[", 8) == 0) {
                tok.pos += 8;
                const char *cdata_start = tok.pos;
                while (tok.pos < tok.end) {
                    if (tok.end - tok.pos >= 3 &&
                        tok.pos[0] == ']' && tok.pos[1] == ']' &&
                        tok.pos[2] == '>') {
                        break;
                    }
                    tok.pos++;
                }
                size_t cdata_len = (size_t)(tok.pos - cdata_start);
                if (tok.pos < tok.end) tok.pos += 3;
                if (stack_top > 0 && cdata_len > 0) {
                    struct dom_node *tn = alloc_node(&pool, DOM_TEXT);
                    if (tn) {
                        size_t copy = cdata_len;
                        if (copy >= sizeof(tn->text)) copy = sizeof(tn->text) - 1;
                        memcpy(tn->text, cdata_start, copy);
                        tn->text[copy] = 0;
                        append_child(stack[stack_top - 1], tn);
                    }
                }
                continue;
            }

            /* Close tag */
            if (*tok.pos == '/') {
                tok.pos++;
                char tag[32];
                read_tag_name(&tok, tag, sizeof(tag));
                while (tok.pos < tok.end && *tok.pos != '>') tok.pos++;
                if (tok.pos < tok.end) tok.pos++;

                if (strcmp(tag, "title") == 0) {
                    in_title = 0;
                    doc->title[title_pos] = 0;
                }
                if (strcmp(tag, "head") == 0)
                    in_head = 0;

                for (int i = stack_top - 1; i >= 0; i--) {
                    if (strcmp(stack[i]->tag, tag) == 0) {
                        stack_top = i;
                        break;
                    }
                }
                continue;
            }

            /* Open tag */
            char tag[32];
            read_tag_name(&tok, tag, sizeof(tag));
            if (tag[0] == 0) {
                while (tok.pos < tok.end && *tok.pos != '>') tok.pos++;
                if (tok.pos < tok.end) tok.pos++;
                continue;
            }

            /* <script> / <style> -- skip their raw content */
            int is_raw = (strcmp(tag, "script") == 0 ||
                          strcmp(tag, "style") == 0);

            /* Auto-close */
            if (stack_top > 0 &&
                should_auto_close(stack[stack_top - 1]->tag, tag))
                stack_top--;

            /* Handle head-section tags */
            if (strcmp(tag, "head") == 0) {
                in_head = 1;
                while (tok.pos < tok.end && *tok.pos != '>') tok.pos++;
                if (tok.pos < tok.end) tok.pos++;
                continue;
            }
            if (strcmp(tag, "html") == 0 || strcmp(tag, "body") == 0) {
                /* skip duplicate structural tags, just consume attrs */
                while (tok.pos < tok.end && *tok.pos != '>') tok.pos++;
                if (tok.pos < tok.end) tok.pos++;
                continue;
            }

            struct dom_node *el = alloc_node(&pool, DOM_ELEMENT);
            if (!el) break;
            strncpy(el->tag, tag, sizeof(el->tag) - 1);

            /* Parse attributes */
            skip_ws(&tok);
            while (tok.pos < tok.end && *tok.pos != '>' && *tok.pos != '/') {
                skip_ws(&tok);
                if (tok.pos >= tok.end || *tok.pos == '>' || *tok.pos == '/')
                    break;
                if (el->attr_count < 16) {
                    struct dom_attr *a = &el->attrs[el->attr_count];
                    read_attr_name(&tok, a->name, sizeof(a->name));
                    if (a->name[0] == 0) { tok.pos++; continue; }
                    skip_ws(&tok);
                    if (tok.pos < tok.end && *tok.pos == '=') {
                        tok.pos++;
                        skip_ws(&tok);
                        read_attr_value(&tok, a->value, sizeof(a->value));
                    } else {
                        a->value[0] = 0;
                    }
                    el->attr_count++;
                } else {
                    while (tok.pos < tok.end && *tok.pos != '>'
                           && !isspace((unsigned char)*tok.pos))
                        tok.pos++;
                }
            }

            int self_close = 0;
            if (tok.pos < tok.end && *tok.pos == '/') {
                self_close = 1;
                tok.pos++;
            }
            if (tok.pos < tok.end && *tok.pos == '>') tok.pos++;

            /* Attach to the tree */
            struct dom_node *target_parent;
            if (in_head || strcmp(tag, "title") == 0 ||
                strcmp(tag, "meta") == 0 || strcmp(tag, "link") == 0 ||
                strcmp(tag, "base") == 0) {
                target_parent = head_el;
            } else if (stack_top > 0) {
                target_parent = stack[stack_top - 1];
            } else {
                target_parent = body_el;
            }
            append_child(target_parent, el);

            if (strcmp(tag, "title") == 0) {
                in_title = 1;
                title_pos = 0;
            }

            if (!self_close && !is_void_tag(tag) && !is_raw) {
                if (stack_top < STACK_MAX)
                    stack[stack_top++] = el;
            }

            /* Skip raw content for <script>/<style> */
            if (is_raw && !self_close) {
                char end_tag[40];
                snprintf(end_tag, sizeof(end_tag), "</%s>", tag);
                size_t et_len = strlen(end_tag);
                const char *raw_start = tok.pos;
                while (tok.pos < tok.end) {
                    if (*tok.pos == '<' &&
                        (size_t)(tok.end - tok.pos) >= et_len) {
                        int match = 1;
                        for (size_t k = 0; k < et_len; k++) {
                            if (tolower((unsigned char)tok.pos[k]) !=
                                tolower((unsigned char)end_tag[k])) {
                                match = 0;
                                break;
                            }
                        }
                        if (match) {
                            /* For <style>, capture content as text child */
                            if (strcmp(tag, "style") == 0) {
                                size_t rlen = (size_t)(tok.pos - raw_start);
                                struct dom_node *tn = alloc_node(&pool,
                                                                  DOM_TEXT);
                                if (tn) {
                                    size_t copy = rlen;
                                    if (copy >= sizeof(tn->text))
                                        copy = sizeof(tn->text) - 1;
                                    memcpy(tn->text, raw_start, copy);
                                    tn->text[copy] = 0;
                                    append_child(el, tn);
                                }
                            }
                            tok.pos += et_len;
                            break;
                        }
                    }
                    tok.pos++;
                }
            }
            continue;
        }

        /* Text content */
        char text_buf[1024];
        int ti = 0;
        while (tok.pos < tok.end && *tok.pos != '<' &&
               ti < (int)sizeof(text_buf) - 1) {
            if (*tok.pos == '&') {
                char ent[8];
                int eaten = 0;
                int n = decode_entity(tok.pos, tok.end, ent, &eaten);
                for (int j = 0; j < n && ti < (int)sizeof(text_buf) - 1; j++)
                    text_buf[ti++] = ent[j];
                tok.pos += eaten;
            } else {
                text_buf[ti++] = *tok.pos;
                tok.pos++;
            }
        }
        text_buf[ti] = 0;

        if (in_title) {
            for (int j = 0; text_buf[j] && title_pos < 255; j++)
                doc->title[title_pos++] = text_buf[j];
        }

        /* Collapse pure-whitespace text between block elements */
        int all_ws = 1;
        for (int j = 0; j < ti; j++) {
            if (!isspace((unsigned char)text_buf[j])) { all_ws = 0; break; }
        }
        if (all_ws && ti > 0) {
            text_buf[0] = ' ';
            text_buf[1] = 0;
            ti = 1;
        }

        if (ti > 0 && stack_top > 0) {
            struct dom_node *tn = alloc_node(&pool, DOM_TEXT);
            if (tn) {
                strncpy(tn->text, text_buf, sizeof(tn->text) - 1);
                tn->text[sizeof(tn->text) - 1] = 0;
                append_child(stack[stack_top - 1], tn);
            }
        }
    }

    doc->node_count = pool.count;
    return doc;
}

void dom_free(struct dom_document *doc)
{
    if (!doc) return;
    if (doc->root) {
        /* All nodes are in a contiguous array starting at the document node.
         * The document node is root->parent. */
        struct dom_node *base = doc->root->parent;
        if (!base) base = doc->root;
        /* Walk to the very first allocation: the doc_node (DOM_DOCUMENT). */
        while (base->parent) base = base->parent;
        free(base);
    }
    free(doc);
}

int dom_get_elements_by_tag(struct dom_node *root, const char *tag,
                            struct dom_node **results, int max)
{
    if (!root || !tag || !results || max <= 0) return 0;
    int count = 0;

    /* Iterative pre-order using an explicit stack to avoid recursion. */
    struct dom_node *stk[512];
    int sp = 0;
    stk[sp++] = root;

    while (sp > 0 && count < max) {
        struct dom_node *n = stk[--sp];
        if (n->type == DOM_ELEMENT && strcmp(n->tag, tag) == 0)
            results[count++] = n;
        /* Push children in reverse order so first child is visited first. */
        struct dom_node *children[256];
        int nc = 0;
        for (struct dom_node *c = n->first_child; c && nc < 256;
             c = c->next_sibling)
            children[nc++] = c;
        for (int i = nc - 1; i >= 0 && sp < 512; i--)
            stk[sp++] = children[i];
    }
    return count;
}

const char *dom_get_attr(const struct dom_node *node, const char *name)
{
    if (!node || !name) return 0;
    for (int i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].name, name) == 0)
            return node->attrs[i].value;
    }
    return 0;
}

int dom_get_text_content(const struct dom_node *node, char *buf, int max)
{
    if (!node || !buf || max <= 0) return 0;
    int pos = 0;

    struct dom_node const *stk[512];
    int sp = 0;
    stk[sp++] = node;

    while (sp > 0) {
        const struct dom_node *n = stk[--sp];
        if (n->type == DOM_TEXT) {
            int tlen = (int)strlen(n->text);
            int copy = tlen;
            if (pos + copy >= max) copy = max - pos - 1;
            if (copy > 0) {
                memcpy(buf + pos, n->text, (size_t)copy);
                pos += copy;
            }
        }
        const struct dom_node *children[256];
        int nc = 0;
        for (struct dom_node *c = n->first_child; c && nc < 256;
             c = c->next_sibling)
            children[nc++] = (const struct dom_node *)c;
        for (int i = nc - 1; i >= 0 && sp < 512; i--)
            stk[sp++] = children[i];
    }
    buf[pos] = 0;
    return pos;
}
