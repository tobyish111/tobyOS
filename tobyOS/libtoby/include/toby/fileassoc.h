#ifndef TOBY_FILEASSOC_H
#define TOBY_FILEASSOC_H

struct file_assoc {
    char extension[8];
    char app_path[128];
    char description[64];
    char icon[32];
};

int         fileassoc_register(const char *ext, const char *app_path,
                               const char *description);
int         fileassoc_unregister(const char *ext);
const char *fileassoc_get_app(const char *ext);
int         fileassoc_get_all(struct file_assoc *out, int max);
int         fileassoc_set_default(const char *ext, const char *app_path);

#endif /* TOBY_FILEASSOC_H */
