/* toby/css.h -- CSS2/3 parser, selector matching, and cascade.
 *
 * Parses stylesheets into rule sets, matches selectors against DOM
 * nodes, computes specificity, and resolves cascaded+inherited styles.
 */

#ifndef TOBY_CSS_H
#define TOBY_CSS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dom_node;

enum css_unit { CSS_PX, CSS_EM, CSS_REM, CSS_PERCENT, CSS_AUTO, CSS_NONE };

struct css_value {
    float number;
    enum css_unit unit;
    uint32_t color;
    char string[64];
    int is_keyword;
};

enum css_prop {
    CSS_PROP_DISPLAY, CSS_PROP_POSITION,
    CSS_PROP_WIDTH, CSS_PROP_HEIGHT,
    CSS_PROP_MARGIN_TOP, CSS_PROP_MARGIN_RIGHT,
    CSS_PROP_MARGIN_BOTTOM, CSS_PROP_MARGIN_LEFT,
    CSS_PROP_PADDING_TOP, CSS_PROP_PADDING_RIGHT,
    CSS_PROP_PADDING_BOTTOM, CSS_PROP_PADDING_LEFT,
    CSS_PROP_BORDER_WIDTH,
    CSS_PROP_COLOR, CSS_PROP_BACKGROUND_COLOR, CSS_PROP_BACKGROUND_IMAGE,
    CSS_PROP_FONT_SIZE, CSS_PROP_FONT_WEIGHT, CSS_PROP_FONT_FAMILY,
    CSS_PROP_TEXT_ALIGN, CSS_PROP_TEXT_DECORATION,
    CSS_PROP_LINE_HEIGHT,
    CSS_PROP_FLOAT, CSS_PROP_CLEAR,
    CSS_PROP_OVERFLOW,
    CSS_PROP_FLEX_DIRECTION, CSS_PROP_JUSTIFY_CONTENT, CSS_PROP_ALIGN_ITEMS,
    CSS_PROP_FLEX_GROW, CSS_PROP_FLEX_SHRINK,
    CSS_PROP_OPACITY,
    CSS_PROP_BORDER_RADIUS,
    CSS_PROP_TOP, CSS_PROP_LEFT, CSS_PROP_RIGHT, CSS_PROP_BOTTOM,
    CSS_PROP_Z_INDEX,
    CSS_PROP_VISIBILITY,
    CSS_PROP_MAX_WIDTH, CSS_PROP_MAX_HEIGHT,
    CSS_PROP_MIN_WIDTH, CSS_PROP_MIN_HEIGHT,
    CSS_PROP_COUNT
};

struct css_declaration {
    enum css_prop property;
    struct css_value value;
};

struct css_selector {
    char text[256];
    int specificity;
};

struct css_rule {
    struct css_selector selector;
    struct css_declaration declarations[32];
    int decl_count;
};

struct css_stylesheet {
    struct css_rule *rules;
    int rule_count;
    int rule_cap;
};

struct computed_style {
    struct css_value props[CSS_PROP_COUNT];
};

struct css_stylesheet *css_parse(const char *css, size_t len);
void css_free(struct css_stylesheet *ss);
int css_selector_matches(const struct css_selector *sel,
                         const struct dom_node *node);
void css_compute_style(const struct css_stylesheet *ss,
                       struct dom_node *node,
                       const struct computed_style *parent_style,
                       struct computed_style *out);
uint32_t css_parse_color(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_CSS_H */
