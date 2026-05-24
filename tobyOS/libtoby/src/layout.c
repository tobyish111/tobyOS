/* libtoby/src/layout.c -- box layout engine for the tobyOS browser. */

#include <toby/layout.h>
#include <toby/css.h>
#include <toby/html.h>
#include <toby/font.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_LAYOUT_BOXES 4096

extern int toby_font_measure_text(toby_font_t *font, const char *text,
                                  int *width, int *height);

/* ------------------------------------------------------------------ */
/*  Inline-element and block-element tables                            */
/* ------------------------------------------------------------------ */

static const char *inline_tags[] = {
    "a","abbr","b","bdo","big","br","cite","code","dfn","em","i",
    "img","input","kbd","label","map","object","q","samp","script",
    "select","small","span","strong","sub","sup","textarea","tt","u",
    "var", 0
};

static int is_inline_tag(const char *tag)
{
    for (int i = 0; inline_tags[i]; i++)
        if (strcmp(tag, inline_tags[i]) == 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Resolve CSS values to pixels                                       */
/* ------------------------------------------------------------------ */

static float resolve_length(const struct css_value *v, float parent_size,
                            float font_size)
{
    if (!v) return 0;
    switch (v->unit) {
    case CSS_PX:      return v->number;
    case CSS_EM:      return v->number * font_size;
    case CSS_REM:     return v->number * 16.0f;
    case CSS_PERCENT: return v->number * parent_size / 100.0f;
    case CSS_AUTO:    return 0;
    case CSS_NONE:    return v->number;
    }
    return 0;
}

static enum layout_type determine_layout_type(const struct computed_style *style,
                                              const struct dom_node *node)
{
    const char *disp = style->props[CSS_PROP_DISPLAY].string;
    if (style->props[CSS_PROP_DISPLAY].is_keyword) {
        if (strcmp(disp, "none") == 0) return LAYOUT_NONE;
        if (strcmp(disp, "flex") == 0) return LAYOUT_FLEX;
        if (strcmp(disp, "inline") == 0 ||
            strcmp(disp, "inline-block") == 0)
            return LAYOUT_INLINE;
    }
    if (node && node->type == DOM_TEXT) return LAYOUT_INLINE;
    if (node && node->type == DOM_ELEMENT && is_inline_tag(node->tag))
        return LAYOUT_INLINE;
    return LAYOUT_BLOCK;
}

/* ------------------------------------------------------------------ */
/*  Build layout tree from DOM                                         */
/* ------------------------------------------------------------------ */

struct build_ctx {
    struct layout_box *boxes;
    int count;
    int cap;
    struct css_stylesheet *ss;
    float vw, vh;
};

static struct layout_box *alloc_box(struct build_ctx *ctx)
{
    if (ctx->count >= ctx->cap) return 0;
    struct layout_box *b = &ctx->boxes[ctx->count++];
    memset(b, 0, sizeof(*b));
    return b;
}

static void box_append_child(struct layout_box *parent, struct layout_box *child)
{
    child->parent = parent;
    child->next_sibling = 0;
    if (!parent->first_child) {
        parent->first_child = child;
    } else {
        struct layout_box *s = parent->first_child;
        while (s->next_sibling) s = s->next_sibling;
        s->next_sibling = child;
    }
}

static struct layout_box *build_box(struct build_ctx *ctx,
                                     struct dom_node *node,
                                     const struct computed_style *parent_style)
{
    if (!node) return 0;

    struct computed_style *style =
        (struct computed_style *)calloc(1, sizeof(struct computed_style));
    if (!style) return 0;

    if (node->type == DOM_TEXT) {
        /* Text nodes inherit parent style, no selector matching */
        if (parent_style)
            memcpy(style, parent_style, sizeof(*style));
    } else {
        css_compute_style(ctx->ss, node, parent_style, style);
    }

    enum layout_type lt = determine_layout_type(style, node);
    if (lt == LAYOUT_NONE) {
        free(style);
        return 0;
    }

    struct layout_box *box = alloc_box(ctx);
    if (!box) { free(style); return 0; }
    box->node = node;
    box->style = style;
    box->type = lt;

    if (node->type == DOM_TEXT) {
        box->text = node->text;
        box->text_len = (int)strlen(node->text);
    }

    /* Build children */
    if (node->type == DOM_ELEMENT) {
        for (struct dom_node *c = node->first_child; c; c = c->next_sibling) {
            /* Skip <script> and <style> */
            if (c->type == DOM_ELEMENT &&
                (strcmp(c->tag, "script") == 0 ||
                 strcmp(c->tag, "style") == 0))
                continue;
            struct layout_box *child_box = build_box(ctx, c, style);
            if (child_box)
                box_append_child(box, child_box);
        }
    }

    return box;
}

struct layout_box *layout_build(struct dom_node *root,
                                struct css_stylesheet *ss,
                                float viewport_width,
                                float viewport_height)
{
    if (!root) return 0;

    struct build_ctx ctx;
    ctx.boxes = (struct layout_box *)calloc(MAX_LAYOUT_BOXES,
                                             sizeof(struct layout_box));
    if (!ctx.boxes) return 0;
    ctx.count = 0;
    ctx.cap = MAX_LAYOUT_BOXES;
    ctx.ss = ss;
    ctx.vw = viewport_width;
    ctx.vh = viewport_height;

    struct computed_style root_style;
    memset(&root_style, 0, sizeof(root_style));
    root_style.props[CSS_PROP_FONT_SIZE].number = 16.0f;
    root_style.props[CSS_PROP_FONT_SIZE].unit = CSS_PX;
    root_style.props[CSS_PROP_COLOR].color = 0xFF000000;
    root_style.props[CSS_PROP_LINE_HEIGHT].number = 1.2f;
    root_style.props[CSS_PROP_LINE_HEIGHT].unit = CSS_EM;

    struct layout_box *root_box = build_box(&ctx, root, &root_style);
    if (root_box) {
        root_box->width = viewport_width;
    }
    return root_box;
}

/* ------------------------------------------------------------------ */
/*  Layout computation                                                 */
/* ------------------------------------------------------------------ */

static float max_f(float a, float b) { return a > b ? a : b; }
static float min_f(float a, float b) { return a < b ? a : b; }

static void resolve_box_model(struct layout_box *box, float parent_width)
{
    if (!box->style) return;
    float fs = box->style->props[CSS_PROP_FONT_SIZE].number;
    if (fs < 1.0f) fs = 16.0f;

    box->margin[0] = resolve_length(&box->style->props[CSS_PROP_MARGIN_TOP],
                                     parent_width, fs);
    box->margin[1] = resolve_length(&box->style->props[CSS_PROP_MARGIN_RIGHT],
                                     parent_width, fs);
    box->margin[2] = resolve_length(&box->style->props[CSS_PROP_MARGIN_BOTTOM],
                                     parent_width, fs);
    box->margin[3] = resolve_length(&box->style->props[CSS_PROP_MARGIN_LEFT],
                                     parent_width, fs);

    box->padding[0] = resolve_length(&box->style->props[CSS_PROP_PADDING_TOP],
                                      parent_width, fs);
    box->padding[1] = resolve_length(&box->style->props[CSS_PROP_PADDING_RIGHT],
                                      parent_width, fs);
    box->padding[2] = resolve_length(&box->style->props[CSS_PROP_PADDING_BOTTOM],
                                      parent_width, fs);
    box->padding[3] = resolve_length(&box->style->props[CSS_PROP_PADDING_LEFT],
                                      parent_width, fs);

    float bw = resolve_length(&box->style->props[CSS_PROP_BORDER_WIDTH],
                               parent_width, fs);
    box->border[0] = box->border[1] = box->border[2] = box->border[3] = bw;
}

static void compute_text_size(struct layout_box *box, float max_width)
{
    if (!box->text || box->text_len == 0) return;
    float fs = 16.0f;
    if (box->style)
        fs = box->style->props[CSS_PROP_FONT_SIZE].number;
    if (fs < 1.0f) fs = 16.0f;

    float char_w = fs * 0.6f;
    float line_h = fs * 1.2f;

    if (max_width < char_w) max_width = char_w;

    int chars_per_line = (int)(max_width / char_w);
    if (chars_per_line < 1) chars_per_line = 1;

    int text_chars = box->text_len;
    int lines = (text_chars + chars_per_line - 1) / chars_per_line;
    if (lines < 1) lines = 1;

    int last_line_chars = text_chars - (lines - 1) * chars_per_line;
    if (last_line_chars <= 0) last_line_chars = text_chars;

    box->content_width = (lines == 1) ? (float)text_chars * char_w
                                      : max_width;
    box->content_height = (float)lines * line_h;
}

static void layout_block(struct layout_box *box, float parent_width);
static void layout_inline_children(struct layout_box *box, float avail_width);
static void layout_flex(struct layout_box *box, float parent_width);

static void layout_box_recursive(struct layout_box *box, float parent_width)
{
    if (!box) return;

    resolve_box_model(box, parent_width);

    float avail = parent_width - box->margin[1] - box->margin[3]
                  - box->padding[1] - box->padding[3]
                  - box->border[1] - box->border[3];

    /* Resolve explicit width */
    if (box->style) {
        struct css_value *wv = &box->style->props[CSS_PROP_WIDTH];
        float fs = box->style->props[CSS_PROP_FONT_SIZE].number;
        if (fs < 1.0f) fs = 16.0f;
        if (wv->unit != CSS_AUTO && wv->unit != CSS_NONE) {
            box->content_width = resolve_length(wv, parent_width, fs);
        } else if (box->type == LAYOUT_BLOCK || box->type == LAYOUT_FLEX) {
            box->content_width = avail;
        }
    } else if (box->type == LAYOUT_BLOCK) {
        box->content_width = avail;
    }

    /* Apply max/min constraints */
    if (box->style) {
        float fs = box->style->props[CSS_PROP_FONT_SIZE].number;
        if (fs < 1.0f) fs = 16.0f;
        struct css_value *maxw = &box->style->props[CSS_PROP_MAX_WIDTH];
        struct css_value *minw = &box->style->props[CSS_PROP_MIN_WIDTH];
        if (maxw->unit != CSS_NONE && maxw->unit != CSS_AUTO && maxw->number > 0) {
            float mw = resolve_length(maxw, parent_width, fs);
            box->content_width = min_f(box->content_width, mw);
        }
        if (minw->unit != CSS_NONE && minw->unit != CSS_AUTO && minw->number > 0) {
            float mw = resolve_length(minw, parent_width, fs);
            box->content_width = max_f(box->content_width, mw);
        }
    }

    /* Text node sizing */
    if (box->text && box->text_len > 0) {
        compute_text_size(box, box->content_width > 0 ? box->content_width : avail);
        box->width = box->content_width + box->padding[1] + box->padding[3]
                     + box->border[1] + box->border[3];
        box->height = box->content_height + box->padding[0] + box->padding[2]
                      + box->border[0] + box->border[2];
        return;
    }

    /* Recurse into children based on layout mode */
    if (box->type == LAYOUT_FLEX) {
        layout_flex(box, parent_width);
    } else {
        /* Check if this block has any inline children */
        int has_inline = 0, has_block = 0;
        for (struct layout_box *c = box->first_child; c; c = c->next_sibling) {
            if (c->type == LAYOUT_INLINE) has_inline = 1;
            else has_block = 1;
        }

        if (has_inline && !has_block) {
            layout_inline_children(box, box->content_width > 0
                                        ? box->content_width : avail);
        } else {
            layout_block(box, parent_width);
        }
    }

    /* Resolve explicit height */
    if (box->style) {
        struct css_value *hv = &box->style->props[CSS_PROP_HEIGHT];
        float fs = box->style->props[CSS_PROP_FONT_SIZE].number;
        if (fs < 1.0f) fs = 16.0f;
        if (hv->unit != CSS_AUTO && hv->unit != CSS_NONE && hv->number > 0) {
            box->content_height = resolve_length(hv, 0, fs);
        }
    }

    box->width = box->content_width + box->padding[1] + box->padding[3]
                 + box->border[1] + box->border[3];
    box->height = box->content_height + box->padding[0] + box->padding[2]
                  + box->border[0] + box->border[2];
}

static void layout_block(struct layout_box *box, float parent_width)
{
    float cw = box->content_width;
    if (cw <= 0) cw = parent_width;
    float y_cursor = 0;
    float prev_margin_bottom = 0;

    for (struct layout_box *c = box->first_child; c; c = c->next_sibling) {
        layout_box_recursive(c, cw);

        /* Margin collapsing: take max of adjacent margins */
        float top_margin = c->margin[0];
        float collapsed = max_f(prev_margin_bottom, top_margin);
        if (c == box->first_child) collapsed = top_margin;

        /* Auto margin centering */
        float x_pos = c->margin[3];
        if (c->style) {
            int left_auto = (c->style->props[CSS_PROP_MARGIN_LEFT].unit == CSS_AUTO);
            int right_auto = (c->style->props[CSS_PROP_MARGIN_RIGHT].unit == CSS_AUTO);
            if (left_auto && right_auto) {
                float total_w = c->width;
                float space = cw - total_w;
                if (space > 0) x_pos = space / 2.0f;
            } else if (left_auto) {
                float space = cw - c->width - c->margin[1];
                if (space > 0) x_pos = space;
            }
        }

        c->x = x_pos + box->padding[3] + box->border[3];
        c->y = y_cursor + collapsed + box->padding[0] + box->border[0];

        y_cursor = c->y - box->padding[0] - box->border[0]
                   + c->height;
        prev_margin_bottom = c->margin[2];
    }

    box->content_height = max_f(box->content_height,
                                y_cursor + prev_margin_bottom);
}

static void layout_inline_children(struct layout_box *box, float avail_width)
{
    if (avail_width <= 0) avail_width = 300;
    float line_x = 0;
    float line_y = 0;
    float line_height = 0;

    for (struct layout_box *c = box->first_child; c; c = c->next_sibling) {
        layout_box_recursive(c, avail_width);

        float cw = c->width + c->margin[1] + c->margin[3];
        /* Wrap to next line if needed */
        if (line_x + cw > avail_width && line_x > 0) {
            line_y += line_height;
            line_x = 0;
            line_height = 0;
        }

        c->x = line_x + c->margin[3] + box->padding[3] + box->border[3];
        c->y = line_y + c->margin[0] + box->padding[0] + box->border[0];

        float ch = c->height + c->margin[0] + c->margin[2];
        if (ch > line_height) line_height = ch;
        line_x += cw;
    }

    box->content_height = max_f(box->content_height,
                                line_y + line_height);
}

static void layout_flex(struct layout_box *box, float parent_width)
{
    float cw = box->content_width;
    if (cw <= 0) cw = parent_width;

    int is_row = 1;
    if (box->style && box->style->props[CSS_PROP_FLEX_DIRECTION].is_keyword &&
        strcmp(box->style->props[CSS_PROP_FLEX_DIRECTION].string, "column") == 0)
        is_row = 0;

    /* First pass: layout children to get their natural sizes */
    float total_main = 0;
    float total_grow = 0;
    int nchildren = 0;
    for (struct layout_box *c = box->first_child; c; c = c->next_sibling) {
        layout_box_recursive(c, cw);
        float cmain = is_row ? (c->width + c->margin[1] + c->margin[3])
                             : (c->height + c->margin[0] + c->margin[2]);
        total_main += cmain;
        float grow = 0;
        if (c->style) grow = c->style->props[CSS_PROP_FLEX_GROW].number;
        total_grow += grow;
        nchildren++;
    }

    float main_size = is_row ? cw : box->content_height;
    float free_space = main_size - total_main;
    if (free_space < 0) free_space = 0;

    /* justify-content */
    float gap = 0, main_offset = 0;
    const char *justify = "";
    if (box->style && box->style->props[CSS_PROP_JUSTIFY_CONTENT].is_keyword)
        justify = box->style->props[CSS_PROP_JUSTIFY_CONTENT].string;

    if (total_grow <= 0) {
        if (strcmp(justify, "center") == 0) {
            main_offset = free_space / 2.0f;
        } else if (strcmp(justify, "flex-end") == 0) {
            main_offset = free_space;
        } else if (strcmp(justify, "space-between") == 0 && nchildren > 1) {
            gap = free_space / (float)(nchildren - 1);
        } else if (strcmp(justify, "space-around") == 0 && nchildren > 0) {
            gap = free_space / (float)nchildren;
            main_offset = gap / 2.0f;
        } else if (strcmp(justify, "space-evenly") == 0 && nchildren > 0) {
            gap = free_space / (float)(nchildren + 1);
            main_offset = gap;
        }
    }

    /* align-items */
    const char *align = "";
    if (box->style && box->style->props[CSS_PROP_ALIGN_ITEMS].is_keyword)
        align = box->style->props[CSS_PROP_ALIGN_ITEMS].string;

    /* Position children */
    float cursor = main_offset;
    float max_cross = 0;

    for (struct layout_box *c = box->first_child; c; c = c->next_sibling) {
        float grow = 0;
        if (c->style) grow = c->style->props[CSS_PROP_FLEX_GROW].number;

        /* Distribute free space to flex-grow items */
        if (grow > 0 && total_grow > 0 && free_space > 0) {
            float extra = free_space * grow / total_grow;
            if (is_row) {
                c->content_width += extra;
                c->width += extra;
            } else {
                c->content_height += extra;
                c->height += extra;
            }
        }

        float pad_left = box->padding[3] + box->border[3];
        float pad_top = box->padding[0] + box->border[0];

        if (is_row) {
            c->x = cursor + c->margin[3] + pad_left;
            c->y = c->margin[0] + pad_top;
            cursor += c->width + c->margin[1] + c->margin[3] + gap;
            float cross = c->height + c->margin[0] + c->margin[2];
            if (cross > max_cross) max_cross = cross;
        } else {
            c->x = c->margin[3] + pad_left;
            c->y = cursor + c->margin[0] + pad_top;
            cursor += c->height + c->margin[0] + c->margin[2] + gap;
            float cross = c->width + c->margin[1] + c->margin[3];
            if (cross > max_cross) max_cross = cross;
        }
    }

    /* Cross-axis alignment */
    float cross_size = is_row ? box->content_height : cw;
    if (cross_size <= 0) cross_size = max_cross;

    for (struct layout_box *c = box->first_child; c; c = c->next_sibling) {
        float pad_left = box->padding[3] + box->border[3];
        float pad_top = box->padding[0] + box->border[0];

        if (is_row) {
            float ch = c->height + c->margin[0] + c->margin[2];
            if (strcmp(align, "center") == 0) {
                c->y = (cross_size - ch) / 2.0f + c->margin[0] + pad_top;
            } else if (strcmp(align, "flex-end") == 0) {
                c->y = cross_size - ch + c->margin[0] + pad_top;
            } else if (strcmp(align, "stretch") == 0) {
                c->height = cross_size - c->margin[0] - c->margin[2];
                c->content_height = c->height - c->padding[0] - c->padding[2]
                                    - c->border[0] - c->border[2];
            }
        } else {
            float cwd = c->width + c->margin[1] + c->margin[3];
            if (strcmp(align, "center") == 0) {
                c->x = (cross_size - cwd) / 2.0f + c->margin[3] + pad_left;
            } else if (strcmp(align, "flex-end") == 0) {
                c->x = cross_size - cwd + c->margin[3] + pad_left;
            } else if (strcmp(align, "stretch") == 0) {
                c->width = cross_size - c->margin[1] - c->margin[3];
                c->content_width = c->width - c->padding[1] - c->padding[3]
                                   - c->border[1] - c->border[3];
            }
        }
    }

    if (is_row) {
        box->content_height = max_f(box->content_height, max_cross);
    } else {
        box->content_height = max_f(box->content_height, cursor);
    }
}

void layout_compute(struct layout_box *root, float viewport_width)
{
    if (!root) return;
    root->x = 0;
    root->y = 0;
    root->content_width = viewport_width;
    layout_box_recursive(root, viewport_width);
}

/* ------------------------------------------------------------------ */
/*  Hit testing                                                        */
/* ------------------------------------------------------------------ */

struct layout_box *layout_hit_test(struct layout_box *root, float px, float py)
{
    if (!root) return 0;

    struct layout_box *stack[1024];
    int sp = 0;
    stack[sp++] = root;
    struct layout_box *best = 0;

    while (sp > 0) {
        struct layout_box *b = stack[--sp];

        /* Compute absolute position by walking up to root */
        float ax = 0, ay = 0;
        for (struct layout_box *p = b; p; p = p->parent) {
            ax += p->x;
            ay += p->y;
        }

        if (px >= ax && px < ax + b->width &&
            py >= ay && py < ay + b->height) {
            best = b;
        }

        for (struct layout_box *c = b->first_child; c && sp < 1024;
             c = c->next_sibling)
            stack[sp++] = c;
    }
    return best;
}

/* ------------------------------------------------------------------ */
/*  Free layout tree                                                   */
/* ------------------------------------------------------------------ */

void layout_free(struct layout_box *root)
{
    if (!root) return;

    /* Free computed styles allocated during build */
    struct layout_box *stack[2048];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        struct layout_box *b = stack[--sp];
        if (b->style) { free(b->style); b->style = 0; }
        for (struct layout_box *c = b->first_child; c && sp < 2048;
             c = c->next_sibling)
            stack[sp++] = c;
    }

    /* All boxes are in a contiguous array; root is the first element. */
    free(root);
}
