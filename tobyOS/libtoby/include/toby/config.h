/* toby/config.h -- Simple key=value config file reader/writer.
 *
 * Provides a lightweight interface for loading, querying, modifying,
 * and saving configuration files with KEY=VALUE line format.
 */

#ifndef TOBY_CONFIG_H
#define TOBY_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_MAX_KEYS    64
#define CONFIG_KEY_MAX     32
#define CONFIG_VALUE_MAX  256
#define CONFIG_PATH_MAX   128

struct config_file {
    char path[CONFIG_PATH_MAX];
    char keys[CONFIG_MAX_KEYS][CONFIG_KEY_MAX];
    char values[CONFIG_MAX_KEYS][CONFIG_VALUE_MAX];
    int  count;
};

int         config_load(struct config_file *cf, const char *path);
int         config_save(const struct config_file *cf);
const char *config_get(const struct config_file *cf, const char *key);
int         config_get_int(const struct config_file *cf, const char *key,
                           int default_val);
void        config_set(struct config_file *cf, const char *key,
                       const char *value);
void        config_set_int(struct config_file *cf, const char *key, int value);
int         config_remove(struct config_file *cf, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_CONFIG_H */
