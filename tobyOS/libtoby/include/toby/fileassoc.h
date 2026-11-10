/* toby/fileassoc.h -- file types and their handlers, for userland.
 *
 * Backed by the single shared table in <tobyos/file_types.h>. This used to
 * carry its own private list of a dozen extensions, which nothing ever
 * called -- see the comment at the top of that header for how many copies of
 * this idea were in the tree.
 *
 * The toby_file_* calls are the ones to use. The older fileassoc_* names are
 * kept because they are public API, and now answer from the same table.
 */

#ifndef TOBY_FILEASSOC_H
#define TOBY_FILEASSOC_H

#include <tobyos/file_types.h>

struct file_assoc {
    char extension[8];
    char app_path[128];
    char description[64];
    char icon[32];
};

/* Look up by FILE NAME or full path (not by bare extension): callers have a
 * name in hand, and making each of them find the dot themselves is how the
 * three previous copies of this code came to disagree about it. Returns 0
 * when the extension is unknown -- an honest "no idea", which the caller
 * should render as "File" rather than guessing. */
const struct file_type *toby_file_type(const char *name);

/* Never return 0: "File" for unknown, so a Type column always has a value. */
const char *toby_file_type_label(const char *name);

/* The app to launch, or 0. A 0 here is not an error -- see toby_file_action:
 * an .elf has no handler because it IS the program. */
const char *toby_file_type_handler(const char *name);

int toby_file_type_action(const char *name);    /* enum file_action */
int toby_file_type_group(const char *name);     /* enum file_group  */

/* Is this safe to load into the text editor? Unknown/extensionless files
 * answer 1: /etc/motd, /etc/hosts and friends carry no extension and are
 * exactly what someone wants to edit. */
int toby_file_type_editable(const char *name);

/* Human-readable group name ("Image", "Audio", ...) for grouping in a UI. */
const char *toby_file_group_name(int group);

/* Total number of known types -- for a gate to assert the table is linked
 * in and not silently empty. */
int toby_file_type_count(void);

/* ---- legacy API, same table underneath ---- */
int         fileassoc_register(const char *ext, const char *app_path,
                               const char *description);
int         fileassoc_unregister(const char *ext);
const char *fileassoc_get_app(const char *ext);
int         fileassoc_get_all(struct file_assoc *out, int max);
int         fileassoc_set_default(const char *ext, const char *app_path);

#endif /* TOBY_FILEASSOC_H */
