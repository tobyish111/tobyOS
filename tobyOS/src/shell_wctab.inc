/* shell_wctab.inc -- tsh's names for musl's wide-character classes and case
 * maps (include/tobyos/wctab.h, generated from the guest's musl). Used only
 * for code points past 0x7f in a UTF-8 locale; ASCII keeps its own rules,
 * which are musl's too. Included by shell.c with the UTF-8 helpers. */
#include <tobyos/wctab.h>

#define SH_WC_ALNUM WC_ALNUM
#define SH_WC_UPPER WC_UPPER
#define SH_WC_LOWER WC_LOWER

static int sh_wc_class_id(const char *name, size_t n) {
    return wc_class_id(name, (unsigned long)n);
}

static bool sh_wc_class(int k, uint32_t c) { return wc_class(k, c) != 0; }
static uint32_t sh_wc_toupper(uint32_t c) { return wc_toupper(c); }
static uint32_t sh_wc_tolower(uint32_t c) { return wc_tolower(c); }
