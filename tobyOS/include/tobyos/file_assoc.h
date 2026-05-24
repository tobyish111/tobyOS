/* file_assoc.h -- File extension to application mapping.
 *
 * Maps file extensions (e.g. "txt", "png") to handler applications
 * so the desktop can "open" files with a double-click or programmatic
 * file_assoc_open() call.
 */

#ifndef TOBYOS_FILE_ASSOC_H
#define TOBYOS_FILE_ASSOC_H

#include <tobyos/types.h>

#define ASSOC_MAX   64
#define FA_EXT_MAX  16
#define FA_PATH_MAX 128

struct file_assoc {
    char extension[FA_EXT_MAX];
    char app_path[FA_PATH_MAX];
    char icon_name[32];
    char mime_type[64];
};

void        file_assoc_init(void);
int         file_assoc_register(const char *ext, const char *app,
                                const char *icon, const char *mime);
const char *file_assoc_get_app(const char *filename);
const char *file_assoc_get_icon(const char *filename);
const char *file_assoc_get_mime(const char *filename);
int         file_assoc_open(const char *filepath);
int         file_assoc_list(struct file_assoc *out, int max);

#endif /* TOBYOS_FILE_ASSOC_H */
