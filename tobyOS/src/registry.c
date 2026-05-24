/* registry.c -- Windows-style registry emulation.
 *
 * Provides HKEY tree (HKLM, HKCU, HKCR) with key-value storage.
 * Supports REG_SZ, REG_DWORD, REG_BINARY, REG_MULTI_SZ types.
 * Persists to /data/system/registry.dat on flush.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/vfs.h>
#include <tobyos/slog.h>

#define REG_MAX_VALUES    256
#define REG_PATH_MAX      128
#define REG_NAME_MAX      64
#define REG_DATA_MAX      256

#define REG_TYPE_NONE      0
#define REG_TYPE_SZ        1
#define REG_TYPE_DWORD     4
#define REG_TYPE_BINARY    3
#define REG_TYPE_MULTI_SZ  7

/* Predefined HKEY handles */
#define HKEY_LOCAL_MACHINE  0x80000002u
#define HKEY_CURRENT_USER   0x80000001u
#define HKEY_CLASSES_ROOT   0x80000000u

struct reg_value {
    bool     active;
    char     path[REG_PATH_MAX];
    char     name[REG_NAME_MAX];
    uint32_t type;
    uint8_t  data[REG_DATA_MAX];
    uint32_t data_len;
};

static struct reg_value g_registry[REG_MAX_VALUES];
static int g_reg_count = 0;
static bool g_initialized = false;
static bool g_dirty = false;

#define REG_PERSIST_PATH  "/data/system/registry.dat"

void registry_init(void) {
    if (g_initialized) return;
    memset(g_registry, 0, sizeof(g_registry));
    g_reg_count = 0;
    g_dirty = false;
    g_initialized = true;
    kprintf("[registry] initialized (max=%d values)\n", REG_MAX_VALUES);
    SLOG_INFO("registry", "registry ready (max=%d)", REG_MAX_VALUES);
}

static const char *hkey_prefix(uint32_t hkey) {
    switch (hkey) {
    case HKEY_LOCAL_MACHINE: return "HKLM";
    case HKEY_CURRENT_USER:  return "HKCU";
    case HKEY_CLASSES_ROOT:  return "HKCR";
    default:                 return "HKEY";
    }
}

static int find_value(const char *full_path, const char *name) {
    for (int i = 0; i < REG_MAX_VALUES; i++) {
        if (!g_registry[i].active) continue;
        if (strcmp(g_registry[i].path, full_path) == 0 &&
            strcmp(g_registry[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int alloc_value(void) {
    for (int i = 0; i < REG_MAX_VALUES; i++) {
        if (!g_registry[i].active) return i;
    }
    return -1;
}

int reg_open_key(uint32_t hkey, const char *subkey, uint32_t *handle_out) {
    if (!subkey || !handle_out) return -1;
    /* Handle is just the hkey + subpath hash for simplicity */
    uint32_t h = hkey ^ 0x12345678u;
    const char *p = subkey;
    while (*p) { h = h * 31 + (uint8_t)*p; p++; }
    *handle_out = h;
    kprintf("[registry] open key: %s\\%s -> handle 0x%08x\n",
            hkey_prefix(hkey), subkey, (unsigned)h);
    return 0;
}

int reg_query_value(uint32_t hkey, const char *subkey,
                    const char *name, uint32_t *type_out,
                    void *data_out, uint32_t *data_len) {
    if (!subkey || !name) return -1;

    char full_path[REG_PATH_MAX];
    ksnprintf(full_path, sizeof(full_path), "%s\\%s", hkey_prefix(hkey), subkey);

    int idx = find_value(full_path, name);
    if (idx < 0) return -1;

    struct reg_value *v = &g_registry[idx];
    if (type_out) *type_out = v->type;
    if (data_out && data_len) {
        uint32_t copy_len = v->data_len;
        if (copy_len > *data_len) copy_len = *data_len;
        memcpy(data_out, v->data, copy_len);
        *data_len = v->data_len;
    }
    return 0;
}

int reg_set_value(uint32_t hkey, const char *subkey,
                  const char *name, uint32_t type,
                  const void *data, uint32_t data_len) {
    if (!subkey || !name || !data) return -1;
    if (data_len > REG_DATA_MAX) return -1;

    char full_path[REG_PATH_MAX];
    ksnprintf(full_path, sizeof(full_path), "%s\\%s", hkey_prefix(hkey), subkey);

    int idx = find_value(full_path, name);
    if (idx < 0) {
        idx = alloc_value();
        if (idx < 0) return -1;
        g_reg_count++;
    }

    struct reg_value *v = &g_registry[idx];
    v->active = true;
    v->type = type;
    v->data_len = data_len;
    memcpy(v->data, data, data_len);

    size_t plen = strlen(full_path);
    if (plen >= REG_PATH_MAX) plen = REG_PATH_MAX - 1;
    memcpy(v->path, full_path, plen);
    v->path[plen] = '\0';

    size_t nlen = strlen(name);
    if (nlen >= REG_NAME_MAX) nlen = REG_NAME_MAX - 1;
    memcpy(v->name, name, nlen);
    v->name[nlen] = '\0';

    g_dirty = true;
    return 0;
}

int reg_delete_key(uint32_t hkey, const char *subkey, const char *name) {
    if (!subkey || !name) return -1;

    char full_path[REG_PATH_MAX];
    ksnprintf(full_path, sizeof(full_path), "%s\\%s", hkey_prefix(hkey), subkey);

    int idx = find_value(full_path, name);
    if (idx < 0) return -1;

    g_registry[idx].active = false;
    g_reg_count--;
    g_dirty = true;
    return 0;
}

int reg_close_key(uint32_t handle) {
    (void)handle;
    return 0;
}

int reg_flush(void) {
    if (!g_dirty) return 0;

    /* Serialize to a simple binary format */
    static uint8_t buf[32768];
    size_t off = 0;

    /* Header: "TREG" + count */
    buf[off++] = 'T'; buf[off++] = 'R'; buf[off++] = 'E'; buf[off++] = 'G';
    uint32_t count = 0;
    for (int i = 0; i < REG_MAX_VALUES; i++) {
        if (g_registry[i].active) count++;
    }
    memcpy(buf + off, &count, 4); off += 4;

    for (int i = 0; i < REG_MAX_VALUES && off < sizeof(buf) - 512; i++) {
        if (!g_registry[i].active) continue;
        struct reg_value *v = &g_registry[i];

        /* path (128) + name (64) + type (4) + data_len (4) + data (var) */
        memcpy(buf + off, v->path, REG_PATH_MAX); off += REG_PATH_MAX;
        memcpy(buf + off, v->name, REG_NAME_MAX); off += REG_NAME_MAX;
        memcpy(buf + off, &v->type, 4); off += 4;
        memcpy(buf + off, &v->data_len, 4); off += 4;
        uint32_t dlen = v->data_len;
        if (dlen > REG_DATA_MAX) dlen = REG_DATA_MAX;
        memcpy(buf + off, v->data, dlen); off += dlen;
    }

    int rc = vfs_write_all(REG_PERSIST_PATH, (const char *)buf, off);
    if (rc == 0) {
        g_dirty = false;
        SLOG_INFO("registry", "flushed %u values to disk", (unsigned)count);
    }
    return rc;
}

int reg_get_count(void) { return g_reg_count; }
