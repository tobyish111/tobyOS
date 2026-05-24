/* libtoby/src/css_parser.c -- CSS tokenizer, selector matcher, cascade. */

#include <toby/css.h>
#include <toby/html.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Tiny float parser (no strtof in libtoby)                           */
/* ------------------------------------------------------------------ */

static float parse_float(const char *s, const char **endp)
{
    float result = 0.0f;
    float sign = 1.0f;
    if (*s == '-') { sign = -1.0f; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9')
        result = result * 10.0f + (float)(*s++ - '0');
    if (*s == '.') {
        s++;
        float frac = 0.1f;
        while (*s >= '0' && *s <= '9') {
            result += (float)(*s++ - '0') * frac;
            frac *= 0.1f;
        }
    }
    if (endp) *endp = s;
    return sign * result;
}

/* ------------------------------------------------------------------ */
/*  Named colors                                                       */
/* ------------------------------------------------------------------ */

struct named_color { const char *name; uint32_t val; };

static const struct named_color named_colors[] = {
    {"black",       0xFF000000}, {"white",       0xFFFFFFFF},
    {"red",         0xFFFF0000}, {"green",       0xFF008000},
    {"blue",        0xFF0000FF}, {"yellow",      0xFFFFFF00},
    {"cyan",        0xFF00FFFF}, {"magenta",     0xFFFF00FF},
    {"orange",      0xFFFFA500}, {"purple",      0xFF800080},
    {"pink",        0xFFFFC0CB}, {"gray",        0xFF808080},
    {"grey",        0xFF808080}, {"silver",      0xFFC0C0C0},
    {"maroon",      0xFF800000}, {"olive",       0xFF808000},
    {"lime",        0xFF00FF00}, {"aqua",        0xFF00FFFF},
    {"teal",        0xFF008080}, {"navy",        0xFF000080},
    {"fuchsia",     0xFFFF00FF}, {"brown",       0xFFA52A2A},
    {"darkgray",    0xFFA9A9A9}, {"darkgrey",    0xFFA9A9A9},
    {"lightgray",   0xFFD3D3D3}, {"lightgrey",   0xFFD3D3D3},
    {"transparent",  0x00000000},
    {0, 0}
};

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

uint32_t css_parse_color(const char *str)
{
    if (!str) return 0xFF000000;
    while (isspace((unsigned char)*str)) str++;

    if (*str == '#') {
        str++;
        size_t len = 0;
        while (isxdigit((unsigned char)str[len])) len++;
        if (len == 3) {
            int r = hex_val(str[0]), g = hex_val(str[1]), b = hex_val(str[2]);
            return 0xFF000000 | (unsigned)((r*17) << 16)
                              | (unsigned)((g*17) << 8)
                              | (unsigned)(b*17);
        }
        if (len == 6) {
            int r = hex_val(str[0])*16 + hex_val(str[1]);
            int g = hex_val(str[2])*16 + hex_val(str[3]);
            int b = hex_val(str[4])*16 + hex_val(str[5]);
            return 0xFF000000 | (unsigned)(r << 16) | (unsigned)(g << 8) | (unsigned)b;
        }
        if (len == 8) {
            int r = hex_val(str[0])*16 + hex_val(str[1]);
            int g = hex_val(str[2])*16 + hex_val(str[3]);
            int b = hex_val(str[4])*16 + hex_val(str[5]);
            int a = hex_val(str[6])*16 + hex_val(str[7]);
            return (unsigned)(a << 24) | (unsigned)(r << 16) | (unsigned)(g << 8)
                   | (unsigned)b;
        }
        return 0xFF000000;
    }

    if (strncmp(str, "rgb", 3) == 0) {
        const char *p = str + 3;
        int has_alpha = 0;
        if (*p == 'a') { has_alpha = 1; p++; }
        if (*p == '(') p++;
        int vals[4] = {0, 0, 0, 255};
        for (int i = 0; i < (has_alpha ? 4 : 3); i++) {
            while (*p && !isdigit((unsigned char)*p) && *p != '.' && *p != ')')
                p++;
            if (*p == ')') break;
            const char *next;
            float fv = parse_float(p, &next);
            if (i == 3 && fv <= 1.0f) fv *= 255.0f;
            vals[i] = (int)fv;
            if (vals[i] < 0) vals[i] = 0;
            if (vals[i] > 255) vals[i] = 255;
            p = next;
        }
        return (unsigned)(vals[3] << 24) | (unsigned)(vals[0] << 16)
               | (unsigned)(vals[1] << 8) | (unsigned)vals[2];
    }

    /* Named color lookup */
    for (int i = 0; named_colors[i].name; i++) {
        const char *cn = named_colors[i].name;
        const char *s = str;
        int match = 1;
        while (*cn) {
            if (tolower((unsigned char)*s) != *cn) { match = 0; break; }
            cn++; s++;
        }
        if (match && (*s == 0 || !isalpha((unsigned char)*s)))
            return named_colors[i].val;
    }
    return 0xFF000000;
}

/* ------------------------------------------------------------------ */
/*  CSS value parser                                                   */
/* ------------------------------------------------------------------ */

static struct css_value parse_css_value(const char *s)
{
    struct css_value v;
    memset(&v, 0, sizeof(v));

    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) { v.unit = CSS_NONE; return v; }

    /* Check keywords first */
    if (strncmp(s, "auto", 4) == 0 && !isalpha((unsigned char)s[4])) {
        v.unit = CSS_AUTO;
        v.is_keyword = 1;
        strncpy(v.string, "auto", sizeof(v.string));
        return v;
    }
    if (strncmp(s, "none", 4) == 0 && !isalpha((unsigned char)s[4])) {
        v.unit = CSS_NONE;
        v.is_keyword = 1;
        strncpy(v.string, "none", sizeof(v.string));
        return v;
    }
    if (strncmp(s, "inherit", 7) == 0) {
        v.unit = CSS_NONE;
        v.is_keyword = 1;
        strncpy(v.string, "inherit", sizeof(v.string));
        return v;
    }

    /* Try color */
    if (*s == '#' || strncmp(s, "rgb", 3) == 0) {
        v.color = css_parse_color(s);
        v.unit = CSS_NONE;
        return v;
    }

    /* Named color check */
    if (isalpha((unsigned char)*s)) {
        for (int i = 0; named_colors[i].name; i++) {
            size_t nlen = strlen(named_colors[i].name);
            if (strncmp(s, named_colors[i].name, nlen) == 0 &&
                !isalpha((unsigned char)s[nlen])) {
                v.color = named_colors[i].val;
                v.unit = CSS_NONE;
                strncpy(v.string, s, sizeof(v.string) - 1);
                return v;
            }
        }
        /* General keyword */
        v.is_keyword = 1;
        v.unit = CSS_NONE;
        int i = 0;
        while (*s && !isspace((unsigned char)*s) && *s != ';' && *s != '}'
               && *s != '!' && i < (int)sizeof(v.string) - 1) {
            v.string[i++] = (char)tolower((unsigned char)*s);
            s++;
        }
        v.string[i] = 0;
        return v;
    }

    /* Numeric value */
    if (isdigit((unsigned char)*s) || *s == '-' || *s == '+' || *s == '.') {
        const char *end;
        v.number = parse_float(s, &end);
        if (strncmp(end, "px", 2) == 0) { v.unit = CSS_PX; }
        else if (strncmp(end, "em", 2) == 0) { v.unit = CSS_EM; }
        else if (strncmp(end, "rem", 3) == 0) { v.unit = CSS_REM; }
        else if (*end == '%') { v.unit = CSS_PERCENT; }
        else { v.unit = CSS_PX; }
        return v;
    }

    /* Fallback: copy as string */
    v.is_keyword = 1;
    v.unit = CSS_NONE;
    strncpy(v.string, s, sizeof(v.string) - 1);
    return v;
}

/* ------------------------------------------------------------------ */
/*  Property name -> enum mapping                                      */
/* ------------------------------------------------------------------ */

struct prop_map { const char *name; enum css_prop id; };

static const struct prop_map prop_table[] = {
    {"display",          CSS_PROP_DISPLAY},
    {"position",         CSS_PROP_POSITION},
    {"width",            CSS_PROP_WIDTH},
    {"height",           CSS_PROP_HEIGHT},
    {"margin-top",       CSS_PROP_MARGIN_TOP},
    {"margin-right",     CSS_PROP_MARGIN_RIGHT},
    {"margin-bottom",    CSS_PROP_MARGIN_BOTTOM},
    {"margin-left",      CSS_PROP_MARGIN_LEFT},
    {"padding-top",      CSS_PROP_PADDING_TOP},
    {"padding-right",    CSS_PROP_PADDING_RIGHT},
    {"padding-bottom",   CSS_PROP_PADDING_BOTTOM},
    {"padding-left",     CSS_PROP_PADDING_LEFT},
    {"border-width",     CSS_PROP_BORDER_WIDTH},
    {"color",            CSS_PROP_COLOR},
    {"background-color", CSS_PROP_BACKGROUND_COLOR},
    {"background-image", CSS_PROP_BACKGROUND_IMAGE},
    {"font-size",        CSS_PROP_FONT_SIZE},
    {"font-weight",      CSS_PROP_FONT_WEIGHT},
    {"font-family",      CSS_PROP_FONT_FAMILY},
    {"text-align",       CSS_PROP_TEXT_ALIGN},
    {"text-decoration",  CSS_PROP_TEXT_DECORATION},
    {"line-height",      CSS_PROP_LINE_HEIGHT},
    {"float",            CSS_PROP_FLOAT},
    {"clear",            CSS_PROP_CLEAR},
    {"overflow",         CSS_PROP_OVERFLOW},
    {"flex-direction",   CSS_PROP_FLEX_DIRECTION},
    {"justify-content",  CSS_PROP_JUSTIFY_CONTENT},
    {"align-items",      CSS_PROP_ALIGN_ITEMS},
    {"flex-grow",        CSS_PROP_FLEX_GROW},
    {"flex-shrink",      CSS_PROP_FLEX_SHRINK},
    {"opacity",          CSS_PROP_OPACITY},
    {"border-radius",    CSS_PROP_BORDER_RADIUS},
    {"top",              CSS_PROP_TOP},
    {"left",             CSS_PROP_LEFT},
    {"right",            CSS_PROP_RIGHT},
    {"bottom",           CSS_PROP_BOTTOM},
    {"z-index",          CSS_PROP_Z_INDEX},
    {"visibility",       CSS_PROP_VISIBILITY},
    {"max-width",        CSS_PROP_MAX_WIDTH},
    {"max-height",       CSS_PROP_MAX_HEIGHT},
    {"min-width",        CSS_PROP_MIN_WIDTH},
    {"min-height",       CSS_PROP_MIN_HEIGHT},
    {0, (enum css_prop)0}
};

static int lookup_prop(const char *name)
{
    for (int i = 0; prop_table[i].name; i++) {
        if (strcmp(name, prop_table[i].name) == 0)
            return (int)prop_table[i].id;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Shorthand expansion                                                */
/* ------------------------------------------------------------------ */

static void add_decl(struct css_rule *rule, enum css_prop prop,
                     struct css_value val)
{
    if (rule->decl_count < 32) {
        rule->declarations[rule->decl_count].property = prop;
        rule->declarations[rule->decl_count].value = val;
        rule->decl_count++;
    }
}

static void expand_shorthand_4(struct css_rule *rule,
                                enum css_prop top, enum css_prop right,
                                enum css_prop bottom, enum css_prop left,
                                const char *value)
{
    /* Parse up to 4 values separated by spaces */
    struct css_value vals[4];
    int nv = 0;
    const char *p = value;
    while (*p && nv < 4) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        char tmp[64];
        size_t tlen = (size_t)(p - start);
        if (tlen >= sizeof(tmp)) tlen = sizeof(tmp) - 1;
        memcpy(tmp, start, tlen);
        tmp[tlen] = 0;
        vals[nv++] = parse_css_value(tmp);
    }
    if (nv == 1) {
        add_decl(rule, top, vals[0]);
        add_decl(rule, right, vals[0]);
        add_decl(rule, bottom, vals[0]);
        add_decl(rule, left, vals[0]);
    } else if (nv == 2) {
        add_decl(rule, top, vals[0]);
        add_decl(rule, bottom, vals[0]);
        add_decl(rule, right, vals[1]);
        add_decl(rule, left, vals[1]);
    } else if (nv == 3) {
        add_decl(rule, top, vals[0]);
        add_decl(rule, right, vals[1]);
        add_decl(rule, left, vals[1]);
        add_decl(rule, bottom, vals[2]);
    } else if (nv == 4) {
        add_decl(rule, top, vals[0]);
        add_decl(rule, right, vals[1]);
        add_decl(rule, bottom, vals[2]);
        add_decl(rule, left, vals[3]);
    }
}

/* ------------------------------------------------------------------ */
/*  Selector specificity                                               */
/* ------------------------------------------------------------------ */

static int calc_specificity(const char *sel)
{
    int ids = 0, classes = 0, elements = 0;
    const char *p = sel;
    while (*p) {
        while (isspace((unsigned char)*p) || *p == '>' || *p == '+' ||
               *p == '~' || *p == ',')
            p++;
        if (*p == '#') { ids++; p++; while (isalnum((unsigned char)*p) || *p == '-' || *p == '_') p++; }
        else if (*p == '.') { classes++; p++; while (isalnum((unsigned char)*p) || *p == '-' || *p == '_') p++; }
        else if (*p == ':') { classes++; p++; while (isalpha((unsigned char)*p) || *p == '-') p++; }
        else if (*p == '[') { classes++; while (*p && *p != ']') p++; if (*p) p++; }
        else if (*p == '*') { p++; }
        else if (isalpha((unsigned char)*p)) {
            elements++;
            while (isalnum((unsigned char)*p) || *p == '-') p++;
        }
        else { p++; }
    }
    return (ids << 16) | (classes << 8) | elements;
}

/* ------------------------------------------------------------------ */
/*  Stylesheet parser                                                  */
/* ------------------------------------------------------------------ */

struct css_stylesheet *css_parse(const char *css, size_t len)
{
    struct css_stylesheet *ss = (struct css_stylesheet *)calloc(1, sizeof(*ss));
    if (!ss) return 0;
    ss->rule_cap = 128;
    ss->rules = (struct css_rule *)calloc((size_t)ss->rule_cap, sizeof(struct css_rule));
    if (!ss->rules) { free(ss); return 0; }

    const char *p = css;
    const char *end = css + len;

    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) break;

        /* Skip comments */
        if (end - p >= 2 && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p < end - 1) {
                if (p[0] == '*' && p[1] == '/') { p += 2; break; }
                p++;
            }
            continue;
        }

        /* Skip @rules */
        if (*p == '@') {
            int brace_depth = 0;
            while (p < end) {
                if (*p == '{') brace_depth++;
                else if (*p == '}') { brace_depth--; if (brace_depth <= 0) { p++; break; } }
                else if (brace_depth == 0 && *p == ';') { p++; break; }
                p++;
            }
            continue;
        }

        /* Read selector(s) up to '{' */
        const char *sel_start = p;
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        size_t sel_len = (size_t)(p - sel_start);
        p++; /* skip '{' */

        /* Trim selector whitespace */
        while (sel_len > 0 && isspace((unsigned char)sel_start[sel_len - 1]))
            sel_len--;

        char sel_text[256];
        if (sel_len >= sizeof(sel_text)) sel_len = sizeof(sel_text) - 1;
        memcpy(sel_text, sel_start, sel_len);
        sel_text[sel_len] = 0;

        /* Read declarations up to '}' */
        const char *decl_start = p;
        int brace = 1;
        while (p < end && brace > 0) {
            if (*p == '{') brace++;
            else if (*p == '}') brace--;
            if (brace > 0) p++;
        }
        const char *decl_end = p;
        if (p < end) p++; /* skip '}' */

        /* Split comma-separated selectors and create one rule per selector */
        char *sel_work = sel_text;
        while (*sel_work) {
            while (isspace((unsigned char)*sel_work)) sel_work++;
            char *comma = sel_work;
            int paren = 0;
            while (*comma) {
                if (*comma == '(') paren++;
                else if (*comma == ')') paren--;
                else if (*comma == ',' && paren == 0) break;
                comma++;
            }
            size_t slen = (size_t)(comma - sel_work);
            while (slen > 0 && isspace((unsigned char)sel_work[slen - 1]))
                slen--;

            if (slen > 0) {
                /* Grow rules array if needed */
                if (ss->rule_count >= ss->rule_cap) {
                    int new_cap = ss->rule_cap * 2;
                    struct css_rule *nr = (struct css_rule *)calloc(
                        (size_t)new_cap, sizeof(struct css_rule));
                    if (!nr) break;
                    memcpy(nr, ss->rules,
                           (size_t)ss->rule_count * sizeof(struct css_rule));
                    free(ss->rules);
                    ss->rules = nr;
                    ss->rule_cap = new_cap;
                }
                struct css_rule *rule = &ss->rules[ss->rule_count];
                memset(rule, 0, sizeof(*rule));
                if (slen >= sizeof(rule->selector.text))
                    slen = sizeof(rule->selector.text) - 1;
                memcpy(rule->selector.text, sel_work, slen);
                rule->selector.text[slen] = 0;
                rule->selector.specificity =
                    calc_specificity(rule->selector.text);

                /* Parse declarations */
                const char *dp = decl_start;
                while (dp < decl_end) {
                    while (dp < decl_end && isspace((unsigned char)*dp)) dp++;
                    if (dp >= decl_end) break;

                    /* Skip comments inside rule body */
                    if (decl_end - dp >= 2 && dp[0] == '/' && dp[1] == '*') {
                        dp += 2;
                        while (dp < decl_end - 1) {
                            if (dp[0] == '*' && dp[1] == '/') {
                                dp += 2;
                                break;
                            }
                            dp++;
                        }
                        continue;
                    }

                    const char *pname_start = dp;
                    while (dp < decl_end && *dp != ':' && *dp != ';') dp++;
                    if (dp >= decl_end || *dp == ';') { if (dp < decl_end) dp++; continue; }
                    size_t pname_len = (size_t)(dp - pname_start);
                    dp++; /* skip ':' */

                    while (pname_len > 0 &&
                           isspace((unsigned char)pname_start[pname_len - 1]))
                        pname_len--;
                    while (pname_len > 0 &&
                           isspace((unsigned char)*pname_start)) {
                        pname_start++;
                        pname_len--;
                    }

                    char pname[64];
                    if (pname_len >= sizeof(pname)) pname_len = sizeof(pname) - 1;
                    for (size_t k = 0; k < pname_len; k++)
                        pname[k] = (char)tolower((unsigned char)pname_start[k]);
                    pname[pname_len] = 0;

                    while (dp < decl_end && isspace((unsigned char)*dp)) dp++;
                    const char *vstart = dp;
                    while (dp < decl_end && *dp != ';' && *dp != '}') dp++;
                    size_t vlen = (size_t)(dp - vstart);
                    while (vlen > 0 && isspace((unsigned char)vstart[vlen - 1]))
                        vlen--;
                    /* Strip !important flag */
                    if (vlen > 10 && strncmp(vstart + vlen - 10, "!important", 10) == 0) {
                        vlen -= 10;
                        while (vlen > 0 && isspace((unsigned char)vstart[vlen - 1]))
                            vlen--;
                    }
                    char vbuf[256];
                    if (vlen >= sizeof(vbuf)) vlen = sizeof(vbuf) - 1;
                    memcpy(vbuf, vstart, vlen);
                    vbuf[vlen] = 0;

                    if (dp < decl_end && *dp == ';') dp++;

                    /* Handle shorthands */
                    if (strcmp(pname, "margin") == 0) {
                        expand_shorthand_4(rule, CSS_PROP_MARGIN_TOP,
                            CSS_PROP_MARGIN_RIGHT, CSS_PROP_MARGIN_BOTTOM,
                            CSS_PROP_MARGIN_LEFT, vbuf);
                        continue;
                    }
                    if (strcmp(pname, "padding") == 0) {
                        expand_shorthand_4(rule, CSS_PROP_PADDING_TOP,
                            CSS_PROP_PADDING_RIGHT, CSS_PROP_PADDING_BOTTOM,
                            CSS_PROP_PADDING_LEFT, vbuf);
                        continue;
                    }

                    int pid = lookup_prop(pname);
                    if (pid >= 0) {
                        struct css_value cv = parse_css_value(vbuf);
                        if (pid == CSS_PROP_COLOR ||
                            pid == CSS_PROP_BACKGROUND_COLOR) {
                            if (cv.color == 0 && !cv.is_keyword)
                                cv.color = css_parse_color(vbuf);
                        }
                        add_decl(rule, (enum css_prop)pid, cv);
                    }
                }
                ss->rule_count++;
            }

            if (*comma == ',') comma++;
            sel_work = comma;
        }
    }
    return ss;
}

void css_free(struct css_stylesheet *ss)
{
    if (!ss) return;
    free(ss->rules);
    free(ss);
}

/* ------------------------------------------------------------------ */
/*  Selector matching                                                  */
/* ------------------------------------------------------------------ */

static int node_has_class(const struct dom_node *node, const char *cls)
{
    const char *class_attr = 0;
    for (int i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].name, "class") == 0) {
            class_attr = node->attrs[i].value;
            break;
        }
    }
    if (!class_attr) return 0;
    size_t clen = strlen(cls);
    const char *p = class_attr;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if ((size_t)(p - s) == clen && strncmp(s, cls, clen) == 0) return 1;
    }
    return 0;
}

static const char *node_get_id(const struct dom_node *node)
{
    for (int i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].name, "id") == 0)
            return node->attrs[i].value;
    }
    return 0;
}

/* Match a single simple selector part (no combinators) against a node. */
static int match_simple(const char *sel, int slen, const struct dom_node *node)
{
    if (node->type != DOM_ELEMENT) return 0;
    const char *p = sel;
    const char *end = sel + slen;
    int any_check = 0;

    while (p < end) {
        if (*p == '#') {
            p++;
            const char *s = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_'))
                p++;
            size_t nlen = (size_t)(p - s);
            const char *nid = node_get_id(node);
            if (!nid || strlen(nid) != nlen || strncmp(nid, s, nlen) != 0)
                return 0;
            any_check = 1;
        } else if (*p == '.') {
            p++;
            const char *s = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_'))
                p++;
            char cls[64];
            size_t nlen = (size_t)(p - s);
            if (nlen >= sizeof(cls)) nlen = sizeof(cls) - 1;
            memcpy(cls, s, nlen);
            cls[nlen] = 0;
            if (!node_has_class(node, cls)) return 0;
            any_check = 1;
        } else if (*p == '*') {
            p++;
            any_check = 1;
        } else if (*p == ':') {
            /* skip pseudo-classes for matching purposes */
            p++;
            while (p < end && (isalpha((unsigned char)*p) || *p == '-')) p++;
            if (p < end && *p == '(') {
                int depth = 1;
                p++;
                while (p < end && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    p++;
                }
            }
        } else if (*p == '[') {
            /* Attribute selector: simplified [attr] or [attr=val] */
            p++;
            char aname[64];
            int ai = 0;
            while (p < end && *p != '=' && *p != ']' &&
                   ai < (int)sizeof(aname) - 1) {
                aname[ai++] = (char)tolower((unsigned char)*p);
                p++;
            }
            aname[ai] = 0;
            if (p < end && *p == '=') {
                p++;
                if (p < end && (*p == '"' || *p == '\'')) p++;
                char aval[256];
                int vi = 0;
                while (p < end && *p != '"' && *p != '\'' && *p != ']' &&
                       vi < (int)sizeof(aval) - 1) {
                    aval[vi++] = *p++;
                }
                aval[vi] = 0;
                if (p < end && (*p == '"' || *p == '\'')) p++;
                if (p < end && *p == ']') p++;
                int found = 0;
                for (int k = 0; k < node->attr_count; k++) {
                    if (strcmp(node->attrs[k].name, aname) == 0 &&
                        strcmp(node->attrs[k].value, aval) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) return 0;
            } else {
                if (p < end && *p == ']') p++;
                int found = 0;
                for (int k = 0; k < node->attr_count; k++) {
                    if (strcmp(node->attrs[k].name, aname) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) return 0;
            }
            any_check = 1;
        } else if (isalpha((unsigned char)*p)) {
            const char *s = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '-')) p++;
            size_t nlen = (size_t)(p - s);
            if (nlen >= 32) nlen = 31;
            char tag[32];
            for (size_t k = 0; k < nlen; k++)
                tag[k] = (char)tolower((unsigned char)s[k]);
            tag[nlen] = 0;
            if (strcmp(node->tag, tag) != 0) return 0;
            any_check = 1;
        } else {
            p++;
        }
    }
    return any_check;
}

int css_selector_matches(const struct css_selector *sel,
                         const struct dom_node *node)
{
    if (!sel || !node || node->type != DOM_ELEMENT) return 0;

    /* Tokenize the selector into parts separated by combinators.
     * We match right-to-left. */
    const char *s = sel->text;
    size_t slen = strlen(s);

    /* Split into segments: each segment is a simple selector, separator is
     * the combinator (' ', '>'). We work from the rightmost segment. */
    struct { int start; int len; char combinator; } parts[32];
    int nparts = 0;

    int i = (int)slen - 1;
    while (i >= 0 && isspace((unsigned char)s[i])) i--;

    while (i >= 0 && nparts < 32) {
        int seg_end = i + 1;
        /* Walk backwards past the simple selector */
        while (i >= 0 && !isspace((unsigned char)s[i]) && s[i] != '>') i--;
        int seg_start = i + 1;
        parts[nparts].start = seg_start;
        parts[nparts].len = seg_end - seg_start;

        /* Now find the combinator */
        while (i >= 0 && isspace((unsigned char)s[i])) i--;
        if (i >= 0 && s[i] == '>') {
            parts[nparts].combinator = '>';
            i--;
            while (i >= 0 && isspace((unsigned char)s[i])) i--;
        } else {
            parts[nparts].combinator = ' ';
        }
        nparts++;
    }

    if (nparts == 0) return 0;

    /* The last segment (parts[0]) must match the target node */
    if (!match_simple(s + parts[0].start, parts[0].len, node)) return 0;

    /* Walk up the tree for remaining segments */
    const struct dom_node *cur = node;
    for (int pi = 1; pi < nparts; pi++) {
        char comb = parts[pi - 1].combinator;
        if (comb == '>') {
            cur = cur->parent;
            if (!cur || !match_simple(s + parts[pi].start, parts[pi].len, cur))
                return 0;
        } else {
            /* Descendant combinator: walk up until we find a match */
            int found = 0;
            cur = cur->parent;
            while (cur) {
                if (match_simple(s + parts[pi].start, parts[pi].len, cur)) {
                    found = 1;
                    break;
                }
                cur = cur->parent;
            }
            if (!found) return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Style computation (cascade + inheritance)                          */
/* ------------------------------------------------------------------ */

static int is_inherited_prop(enum css_prop p)
{
    return p == CSS_PROP_COLOR || p == CSS_PROP_FONT_SIZE ||
           p == CSS_PROP_FONT_WEIGHT || p == CSS_PROP_FONT_FAMILY ||
           p == CSS_PROP_TEXT_ALIGN || p == CSS_PROP_TEXT_DECORATION ||
           p == CSS_PROP_LINE_HEIGHT || p == CSS_PROP_VISIBILITY;
}

static void apply_defaults(struct computed_style *out)
{
    out->props[CSS_PROP_DISPLAY].is_keyword = 1;
    strncpy(out->props[CSS_PROP_DISPLAY].string, "block", 64);
    out->props[CSS_PROP_DISPLAY].unit = CSS_NONE;

    out->props[CSS_PROP_COLOR].color = 0xFF000000;
    out->props[CSS_PROP_BACKGROUND_COLOR].color = 0x00000000;
    out->props[CSS_PROP_FONT_SIZE].number = 16.0f;
    out->props[CSS_PROP_FONT_SIZE].unit = CSS_PX;
    out->props[CSS_PROP_LINE_HEIGHT].number = 1.2f;
    out->props[CSS_PROP_LINE_HEIGHT].unit = CSS_EM;
    out->props[CSS_PROP_OPACITY].number = 1.0f;
    out->props[CSS_PROP_OPACITY].unit = CSS_PX;
    out->props[CSS_PROP_VISIBILITY].is_keyword = 1;
    strncpy(out->props[CSS_PROP_VISIBILITY].string, "visible", 64);
}

void css_compute_style(const struct css_stylesheet *ss,
                       struct dom_node *node,
                       const struct computed_style *parent_style,
                       struct computed_style *out)
{
    memset(out, 0, sizeof(*out));
    apply_defaults(out);

    /* Inherit from parent */
    if (parent_style) {
        for (int p = 0; p < CSS_PROP_COUNT; p++) {
            if (is_inherited_prop((enum css_prop)p))
                out->props[p] = parent_style->props[p];
        }
    }

    if (!ss || !node || node->type != DOM_ELEMENT) {
        node->computed_style = out;
        return;
    }

    /* Collect matching rules, sorted by specificity (stable order) */
    struct { int rule_idx; int spec; } matches[256];
    int nmatch = 0;
    for (int r = 0; r < ss->rule_count && nmatch < 256; r++) {
        if (css_selector_matches(&ss->rules[r].selector, node)) {
            matches[nmatch].rule_idx = r;
            matches[nmatch].spec = ss->rules[r].selector.specificity;
            nmatch++;
        }
    }

    /* Sort by specificity (simple insertion sort) */
    for (int i = 1; i < nmatch; i++) {
        int j = i;
        while (j > 0 && matches[j].spec < matches[j-1].spec) {
            int ti = matches[j].rule_idx;
            int ts = matches[j].spec;
            matches[j].rule_idx = matches[j-1].rule_idx;
            matches[j].spec = matches[j-1].spec;
            matches[j-1].rule_idx = ti;
            matches[j-1].spec = ts;
            j--;
        }
    }

    /* Apply declarations in order (lower specificity first) */
    for (int m = 0; m < nmatch; m++) {
        const struct css_rule *rule = &ss->rules[matches[m].rule_idx];
        for (int d = 0; d < rule->decl_count; d++) {
            const struct css_declaration *decl = &rule->declarations[d];
            if (decl->value.is_keyword &&
                strcmp(decl->value.string, "inherit") == 0) {
                if (parent_style)
                    out->props[decl->property] =
                        parent_style->props[decl->property];
            } else {
                out->props[decl->property] = decl->value;
            }
        }
    }

    /* Inline style attribute */
    const char *style_attr = 0;
    for (int i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].name, "style") == 0) {
            style_attr = node->attrs[i].value;
            break;
        }
    }
    if (style_attr) {
        const char *dp = style_attr;
        const char *dend = style_attr + strlen(style_attr);
        while (dp < dend) {
            while (dp < dend && isspace((unsigned char)*dp)) dp++;
            const char *pn = dp;
            while (dp < dend && *dp != ':' && *dp != ';') dp++;
            if (dp >= dend || *dp == ';') { if (dp < dend) dp++; continue; }
            size_t pnl = (size_t)(dp - pn);
            dp++;
            while (pnl > 0 && isspace((unsigned char)pn[pnl-1])) pnl--;
            while (pnl > 0 && isspace((unsigned char)*pn)) { pn++; pnl--; }
            char pname[64];
            if (pnl >= sizeof(pname)) pnl = sizeof(pname) - 1;
            for (size_t k = 0; k < pnl; k++)
                pname[k] = (char)tolower((unsigned char)pn[k]);
            pname[pnl] = 0;

            while (dp < dend && isspace((unsigned char)*dp)) dp++;
            const char *vs = dp;
            while (dp < dend && *dp != ';') dp++;
            size_t vl = (size_t)(dp - vs);
            while (vl > 0 && isspace((unsigned char)vs[vl-1])) vl--;
            char vbuf[256];
            if (vl >= sizeof(vbuf)) vl = sizeof(vbuf) - 1;
            memcpy(vbuf, vs, vl);
            vbuf[vl] = 0;
            if (dp < dend) dp++;

            int pid = lookup_prop(pname);
            if (pid >= 0) {
                struct css_value cv = parse_css_value(vbuf);
                if (pid == CSS_PROP_COLOR || pid == CSS_PROP_BACKGROUND_COLOR) {
                    if (cv.color == 0 && !cv.is_keyword)
                        cv.color = css_parse_color(vbuf);
                }
                out->props[pid] = cv;
            }
        }
    }

    node->computed_style = out;
}
