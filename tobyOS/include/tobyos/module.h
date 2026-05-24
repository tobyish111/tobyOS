/* module.h -- loadable kernel module (LKM) system for tobyOS.
 *
 * Modules are ELF relocatable objects (.ko files) placed in
 * /lib/modules/. They export init/fini via special ELF sections
 * and run at ring 0 with full kernel privileges.
 */

#ifndef TOBYOS_MODULE_H
#define TOBYOS_MODULE_H

#include <tobyos/types.h>

#define MODULE_MAX       32
#define MODULE_NAME_MAX  32

/* Loaded module tracking. */
struct loaded_module {
    bool     in_use;
    char     name[MODULE_NAME_MAX];
    void    *base;
    size_t   size;
    int    (*init)(void);
    void   (*fini)(void);
    bool     initialized;
};

/* Module loader API. */
void module_init(void);
int  module_load(const char *path);
int  module_unload(const char *name);
int  module_load_all(void);
void module_list(void);
struct loaded_module *module_find(const char *name);

/* Macros for module metadata -- placed in special ELF sections. */
#define TOBY_MODULE_INIT(fn) \
    __attribute__((used, section(".toby_mod_init"))) \
    static int (*_toby_mod_init)(void) = (fn)

#define TOBY_MODULE_FINI(fn) \
    __attribute__((used, section(".toby_mod_fini"))) \
    static void (*_toby_mod_fini)(void) = (fn)

#define MODULE_NAME(n) \
    __attribute__((used, section(".toby_mod_name"))) \
    static const char _toby_mod_name[] = (n)

#define MODULE_VERSION(v) \
    __attribute__((used, section(".toby_mod_ver"))) \
    static const char _toby_mod_ver[] = (v)

#endif /* TOBYOS_MODULE_H */
