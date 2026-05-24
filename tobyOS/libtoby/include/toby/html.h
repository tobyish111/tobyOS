/* toby/html.h -- simplified HTML5 parser producing a DOM tree.
 *
 * Tokenizes and tree-builds a subset of HTML5 sufficient for rendering
 * web pages in the tobyOS browser: elements, text, attributes, void
 * elements, entity decoding, auto-closing, and <title> extraction.
 */

#ifndef TOBY_HTML_H
#define TOBY_HTML_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dom_node_type { DOM_ELEMENT = 1, DOM_TEXT = 3, DOM_DOCUMENT = 9 };

struct dom_attr { char name[64]; char value[256]; };

struct dom_node {
    enum dom_node_type type;
    char tag[32];
    char text[1024];
    struct dom_attr attrs[16];
    int attr_count;
    struct dom_node *parent;
    struct dom_node *first_child;
    struct dom_node *next_sibling;
    void *computed_style;
};

struct dom_document {
    struct dom_node *root;
    struct dom_node *head;
    struct dom_node *body;
    char title[256];
    int node_count;
};

struct dom_document *html_parse(const char *html, size_t len);
void dom_free(struct dom_document *doc);
int dom_get_elements_by_tag(struct dom_node *root, const char *tag,
                            struct dom_node **results, int max);
const char *dom_get_attr(const struct dom_node *node, const char *name);
int dom_get_text_content(const struct dom_node *node, char *buf, int max);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_HTML_H */
