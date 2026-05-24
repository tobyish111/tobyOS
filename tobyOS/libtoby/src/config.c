/* config.c -- Simple key=value configuration file reader/writer.
 *
 * File format:
 *   - Lines of the form KEY=VALUE
 *   - Lines starting with '#' are comments (skipped on load, not preserved)
 *   - Empty lines are skipped
 *   - Leading/trailing whitespace in key and value is trimmed
 */

#include "libtoby_internal.h"
#include <toby/config.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

/* ---- Helpers ---- */

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *skip_space(const char *s)
{
    while (*s && is_space(*s)) s++;
    return s;
}

/* ---- Load ---- */

int config_load(struct config_file *cf, const char *path)
{
    if (!cf || !path) return -1;

    memset(cf, 0, sizeof(*cf));
    size_t plen = strlen(path);
    if (plen >= CONFIG_PATH_MAX) plen = CONFIG_PATH_MAX - 1;
    memcpy(cf->path, path, plen);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    char buf[4096];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, sizeof(buf) - (size_t)total - 1)) > 0) {
        total += n;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    close(fd);
    buf[total] = '\0';

    /* Parse line by line */
    char *line = buf;
    while (*line && cf->count < CONFIG_MAX_KEYS) {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;

        char saved = *eol;
        *eol = '\0';

        const char *trimmed = skip_space(line);
        if (*trimmed && *trimmed != '#') {
            /* Find the '=' separator */
            const char *eq = trimmed;
            while (*eq && *eq != '=') eq++;
            if (*eq == '=') {
                /* Key */
                size_t klen = (size_t)(eq - trimmed);
                while (klen > 0 && is_space(trimmed[klen - 1])) klen--;
                if (klen >= CONFIG_KEY_MAX) klen = CONFIG_KEY_MAX - 1;
                memcpy(cf->keys[cf->count], trimmed, klen);
                cf->keys[cf->count][klen] = '\0';

                /* Value */
                const char *val = skip_space(eq + 1);
                size_t vlen = strlen(val);
                while (vlen > 0 && is_space(val[vlen - 1])) vlen--;
                if (vlen >= CONFIG_VALUE_MAX) vlen = CONFIG_VALUE_MAX - 1;
                memcpy(cf->values[cf->count], val, vlen);
                cf->values[cf->count][vlen] = '\0';

                cf->count++;
            }
        }

        if (saved == '\0') break;
        line = eol + 1;
    }

    return 0;
}

/* ---- Save ---- */

int config_save(const struct config_file *cf)
{
    if (!cf || !cf->path[0]) return -1;

    int fd = open(cf->path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;

    for (int i = 0; i < cf->count; i++) {
        char line[CONFIG_KEY_MAX + CONFIG_VALUE_MAX + 4];
        int len = 0;

        const char *k = cf->keys[i];
        while (*k && len < (int)sizeof(line) - 3) line[len++] = *k++;
        line[len++] = '=';
        const char *v = cf->values[i];
        while (*v && len < (int)sizeof(line) - 2) line[len++] = *v++;
        line[len++] = '\n';

        write(fd, line, (size_t)len);
    }

    close(fd);
    return 0;
}

/* ---- Get ---- */

const char *config_get(const struct config_file *cf, const char *key)
{
    if (!cf || !key) return NULL;
    for (int i = 0; i < cf->count; i++) {
        if (strcmp(cf->keys[i], key) == 0)
            return cf->values[i];
    }
    return NULL;
}

int config_get_int(const struct config_file *cf, const char *key,
                   int default_val)
{
    const char *v = config_get(cf, key);
    if (!v) return default_val;

    int result = 0;
    int sign = 1;
    const char *p = v;
    if (*p == '-') { sign = -1; p++; }
    while (*p >= '0' && *p <= '9') {
        result = result * 10 + (*p - '0');
        p++;
    }
    return result * sign;
}

/* ---- Set ---- */

void config_set(struct config_file *cf, const char *key, const char *value)
{
    if (!cf || !key || !value) return;

    /* Update existing */
    for (int i = 0; i < cf->count; i++) {
        if (strcmp(cf->keys[i], key) == 0) {
            size_t vlen = strlen(value);
            if (vlen >= CONFIG_VALUE_MAX) vlen = CONFIG_VALUE_MAX - 1;
            memcpy(cf->values[i], value, vlen);
            cf->values[i][vlen] = '\0';
            return;
        }
    }

    /* Append new */
    if (cf->count >= CONFIG_MAX_KEYS) return;
    size_t klen = strlen(key);
    if (klen >= CONFIG_KEY_MAX) klen = CONFIG_KEY_MAX - 1;
    memcpy(cf->keys[cf->count], key, klen);
    cf->keys[cf->count][klen] = '\0';

    size_t vlen = strlen(value);
    if (vlen >= CONFIG_VALUE_MAX) vlen = CONFIG_VALUE_MAX - 1;
    memcpy(cf->values[cf->count], value, vlen);
    cf->values[cf->count][vlen] = '\0';

    cf->count++;
}

void config_set_int(struct config_file *cf, const char *key, int value)
{
    char buf[32];
    int neg = 0;
    unsigned int uv;

    if (value < 0) {
        neg = 1;
        uv = (unsigned int)(-(value + 1)) + 1;
    } else {
        uv = (unsigned int)value;
    }

    int pos = (int)sizeof(buf) - 1;
    buf[pos] = '\0';
    do {
        buf[--pos] = '0' + (char)(uv % 10);
        uv /= 10;
    } while (uv > 0);
    if (neg) buf[--pos] = '-';

    config_set(cf, key, buf + pos);
}

/* ---- Remove ---- */

int config_remove(struct config_file *cf, const char *key)
{
    if (!cf || !key) return -1;
    for (int i = 0; i < cf->count; i++) {
        if (strcmp(cf->keys[i], key) == 0) {
            /* Shift remaining entries */
            for (int j = i; j < cf->count - 1; j++) {
                memcpy(cf->keys[j], cf->keys[j + 1], CONFIG_KEY_MAX);
                memcpy(cf->values[j], cf->values[j + 1], CONFIG_VALUE_MAX);
            }
            cf->count--;
            return 0;
        }
    }
    return -1;
}
