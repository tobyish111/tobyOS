/* devmgr.c -- Device manager: PCI enumeration and device tree.
 *
 * Enumerates all PCI devices, creates a device tree, tracks driver
 * binding status, and exposes via a sysfs-like interface.
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pci.h>
#include <tobyos/slog.h>

#define DEVMGR_MAX_NODES    128
#define DEVMGR_NAME_MAX     32
#define DEVMGR_DRIVER_MAX   16
#define DEVMGR_MAX_CHILDREN 8

enum device_type {
    DEV_TYPE_UNKNOWN = 0,
    DEV_TYPE_PCI,
    DEV_TYPE_USB,
    DEV_TYPE_PLATFORM,
    DEV_TYPE_VIRTUAL,
};

enum device_status {
    DEV_STATUS_DISABLED = 0,
    DEV_STATUS_ENABLED,
    DEV_STATUS_ERROR,
    DEV_STATUS_SUSPENDED,
};

struct device_node {
    bool             active;
    char             name[DEVMGR_NAME_MAX];
    enum device_type type;
    enum device_status status;
    char             driver[DEVMGR_DRIVER_MAX];
    int              parent_idx;
    int              children[DEVMGR_MAX_CHILDREN];
    int              child_count;
    uint16_t         vendor_id;
    uint16_t         device_id;
    uint8_t          bus;
    uint8_t          slot;
    uint8_t          func;
    uint8_t          class_code;
    uint8_t          subclass;
};

static struct device_node g_nodes[DEVMGR_MAX_NODES];
static int g_node_count = 0;
static bool g_initialized = false;

static int alloc_node(void) {
    if (g_node_count >= DEVMGR_MAX_NODES) return -1;
    int idx = g_node_count++;
    memset(&g_nodes[idx], 0, sizeof(g_nodes[idx]));
    g_nodes[idx].active = true;
    g_nodes[idx].parent_idx = -1;
    g_nodes[idx].status = DEV_STATUS_ENABLED;
    for (int i = 0; i < DEVMGR_MAX_CHILDREN; i++)
        g_nodes[idx].children[i] = -1;
    return idx;
}

static void add_child(int parent_idx, int child_idx) {
    if (parent_idx < 0 || parent_idx >= g_node_count) return;
    struct device_node *p = &g_nodes[parent_idx];
    if (p->child_count >= DEVMGR_MAX_CHILDREN) return;
    p->children[p->child_count++] = child_idx;
    g_nodes[child_idx].parent_idx = parent_idx;
}

void devmgr_init(void) {
    if (g_initialized) return;
    memset(g_nodes, 0, sizeof(g_nodes));
    g_node_count = 0;

    /* Create root node */
    int root = alloc_node();
    if (root >= 0) {
        ksnprintf(g_nodes[root].name, DEVMGR_NAME_MAX, "system");
        g_nodes[root].type = DEV_TYPE_PLATFORM;
    }

    g_initialized = true;
    kprintf("[devmgr] device manager initialized\n");
    SLOG_INFO("devmgr", "device manager ready (max=%d nodes)", DEVMGR_MAX_NODES);
}

void devmgr_enumerate(void) {
    if (!g_initialized) return;

    int pci_root = alloc_node();
    if (pci_root < 0) return;
    ksnprintf(g_nodes[pci_root].name, DEVMGR_NAME_MAX, "pci");
    g_nodes[pci_root].type = DEV_TYPE_PCI;
    add_child(0, pci_root);

    /* Walk PCI bus 0 */
    for (int slot = 0; slot < 32; slot++) {
        for (int func = 0; func < 8; func++) {
            uint32_t id = pci_cfg_read32(0, (uint8_t)slot, (uint8_t)func, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)((id >> 16) & 0xFFFF);

            if (vendor == 0xFFFF || vendor == 0x0000) continue;

            uint32_t class_reg = pci_cfg_read32(0, (uint8_t)slot, (uint8_t)func, 0x08);
            uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFF);
            uint8_t subclass = (uint8_t)((class_reg >> 16) & 0xFF);

            int node = alloc_node();
            if (node < 0) break;

            struct device_node *n = &g_nodes[node];
            ksnprintf(n->name, DEVMGR_NAME_MAX, "00:%02x.%d", slot, func);
            n->type = DEV_TYPE_PCI;
            n->vendor_id = vendor;
            n->device_id = device;
            n->bus = 0;
            n->slot = (uint8_t)slot;
            n->func = (uint8_t)func;
            n->class_code = class_code;
            n->subclass = subclass;
            n->driver[0] = '\0';
            add_child(pci_root, node);
        }
    }

    kprintf("[devmgr] enumerated %d device nodes\n", g_node_count);
    SLOG_INFO("devmgr", "PCI enumeration complete: %d nodes", g_node_count);
}

int devmgr_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < g_node_count; i++) {
        if (!g_nodes[i].active) continue;
        if (g_nodes[i].vendor_id == vendor && g_nodes[i].device_id == device)
            return i;
    }
    return -1;
}

int devmgr_enable(int node_idx) {
    if (node_idx < 0 || node_idx >= g_node_count) return -1;
    if (!g_nodes[node_idx].active) return -1;
    g_nodes[node_idx].status = DEV_STATUS_ENABLED;
    SLOG_INFO("devmgr", "enabled device '%s'", g_nodes[node_idx].name);
    return 0;
}

int devmgr_disable(int node_idx) {
    if (node_idx < 0 || node_idx >= g_node_count) return -1;
    if (!g_nodes[node_idx].active) return -1;
    g_nodes[node_idx].status = DEV_STATUS_DISABLED;
    SLOG_INFO("devmgr", "disabled device '%s'", g_nodes[node_idx].name);
    return 0;
}

int devmgr_bind_driver(int node_idx, const char *driver_name) {
    if (node_idx < 0 || node_idx >= g_node_count) return -1;
    if (!g_nodes[node_idx].active || !driver_name) return -1;
    size_t len = strlen(driver_name);
    if (len >= DEVMGR_DRIVER_MAX) len = DEVMGR_DRIVER_MAX - 1;
    memcpy(g_nodes[node_idx].driver, driver_name, len);
    g_nodes[node_idx].driver[len] = '\0';
    return 0;
}

const char *devmgr_get_name(int node_idx) {
    if (node_idx < 0 || node_idx >= g_node_count) return NULL;
    return g_nodes[node_idx].name;
}

int devmgr_get_count(void) { return g_node_count; }
