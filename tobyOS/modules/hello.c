/* modules/hello.c -- the smallest loadable kernel module, for insmod/lsmod/
 * rmmod/modprobe to have something REAL to load.
 *
 * The module subsystem (src/module.c) has been a working ET_REL loader with
 * a kernel symbol table since it was written, and nothing ever built a
 * module for it: `[module] /lib/modules not found` at every boot. This is
 * built with the kernel's own flags as an object file (never linked), staged
 * as /lib/modules/hello.ko, and the distro gate loads, lists, unloads and
 * modprobes it. It uses exactly one exported symbol, kprintf, so it also
 * proves the relocation path against the ksym table.
 *
 * On load it prints one line; on unload another. That is the whole module,
 * and it is enough: the point is the loader, not the module. */
#include <tobyos/module.h>
#include <tobyos/printk.h>

MODULE_NAME("hello");

static int hello_count;

static int hello_init(void) {
    hello_count++;
    kprintf("[hello] module loaded (load #%d)\n", hello_count);
    return 0;
}

static void hello_fini(void) {
    kprintf("[hello] module unloaded\n");
}

TOBY_MODULE_INIT(hello_init);
TOBY_MODULE_FINI(hello_fini);
