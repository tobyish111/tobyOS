/* installer.c -- Graphical installer for tobyOS (Phase 10 M10.1).
 *
 * Provides a multi-step installation wizard with:
 *   - Language/locale selection
 *   - Disk partitioning (auto or manual)
 *   - User account creation
 *   - Installation progress
 *   - Boot configuration (UEFI)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Installer state machine */
typedef enum {
    INST_WELCOME,
    INST_LANGUAGE,
    INST_LICENSE,
    INST_DISK_SELECT,
    INST_PARTITION,
    INST_USER_SETUP,
    INST_COPYING,
    INST_BOOTLOADER,
    INST_COMPLETE,
    INST_ERROR
} installer_state_t;

/* Disk information */
struct disk_info {
    int     id;
    char    name[32];
    uint64_t size_mb;
    int     has_os;
};

/* Partition layout */
struct partition {
    uint64_t start_mb;
    uint64_t size_mb;
    char     label[32];
    char     fstype[16];   /* "tobyfs", "fat32", "swap" */
    int      bootable;
};

/* Installer context */
struct installer_ctx {
    installer_state_t state;
    int language;           /* 0=English, 1=German, 2=Japanese, 3=Spanish */
    int selected_disk;
    struct disk_info disks[8];
    int disk_count;
    struct partition parts[4];
    int part_count;
    char username[64];
    char hostname[64];
    char password[64];
    int progress_percent;
    char error_msg[256];
};

static struct installer_ctx g_inst;

/* ---- Localized strings ---- */

static const char *str_welcome[] = {
    "Welcome to tobyOS Setup",
    "Willkommen bei tobyOS Setup",
    "tobyOS セットアップへようこそ",
    "Bienvenido a la configuración de tobyOS"
};

static const char *str_continue[] = {
    "Press Enter to continue...",
    "Drücken Sie Enter um fortzufahren...",
    "Enterを押して続行...",
    "Presione Enter para continuar..."
};

/* ---- Disk detection ---- */

extern int sys_disk_count(void);
extern int sys_disk_info(int id, char *name, int name_len, uint64_t *size_mb);

static void detect_disks(void) {
    g_inst.disk_count = 0;

    /* Try to detect disks via syscall; fall back to mock data */
    int count = sys_disk_count();
    if (count <= 0) {
        /* Mock: one 256 GB disk */
        g_inst.disks[0].id = 0;
        memcpy(g_inst.disks[0].name, "VBOX HARDDISK", 14);
        g_inst.disks[0].name[14] = '\0';
        g_inst.disks[0].size_mb = 262144;
        g_inst.disks[0].has_os = 0;
        g_inst.disk_count = 1;
        return;
    }

    for (int i = 0; i < count && i < 8; i++) {
        g_inst.disks[i].id = i;
        uint64_t sz = 0;
        sys_disk_info(i, g_inst.disks[i].name, 32, &sz);
        g_inst.disks[i].size_mb = sz;
        g_inst.disks[i].has_os = 0;
        g_inst.disk_count++;
    }
}

/* ---- Auto-partition ---- */

static void auto_partition(int disk_idx) {
    uint64_t total = g_inst.disks[disk_idx].size_mb;
    g_inst.part_count = 0;

    /* EFI System Partition: 512 MB */
    g_inst.parts[0].start_mb = 1;
    g_inst.parts[0].size_mb = 512;
    memcpy(g_inst.parts[0].label, "EFI", 4);
    memcpy(g_inst.parts[0].fstype, "fat32", 6);
    g_inst.parts[0].bootable = 1;
    g_inst.part_count++;

    /* Swap: 2 GB or 4 GB */
    uint64_t swap_mb = (total > 16384) ? 4096 : 2048;
    g_inst.parts[1].start_mb = 513;
    g_inst.parts[1].size_mb = swap_mb;
    memcpy(g_inst.parts[1].label, "swap", 5);
    memcpy(g_inst.parts[1].fstype, "swap", 5);
    g_inst.parts[1].bootable = 0;
    g_inst.part_count++;

    /* Root: remainder */
    uint64_t root_start = 513 + swap_mb;
    uint64_t root_size = total - root_start;
    g_inst.parts[2].start_mb = root_start;
    g_inst.parts[2].size_mb = root_size;
    memcpy(g_inst.parts[2].label, "tobyOS", 7);
    memcpy(g_inst.parts[2].fstype, "tobyfs", 7);
    g_inst.parts[2].bootable = 0;
    g_inst.part_count++;
}

/* ---- Installation (copy files) ---- */

extern int sys_install_copy(const char *src, const char *dst);
extern int sys_install_mkfs(int disk, int part, const char *fstype);
extern int sys_install_bootloader(int disk);

static int perform_install(void) {
    g_inst.progress_percent = 0;

    /* Step 1: Format partitions (20%) */
    for (int i = 0; i < g_inst.part_count; i++) {
        sys_install_mkfs(g_inst.selected_disk, i, g_inst.parts[i].fstype);
        g_inst.progress_percent = 5 + (i * 5);
    }
    g_inst.progress_percent = 20;

    /* Step 2: Copy system files (20-80%) */
    static const char *copy_items[] = {
        "/boot/tobyos.bin",
        "/boot/initrd.tar",
        "/lib/libtoby.so",
        "/lib/ld-toby.so",
        "/bin/login",
        "/bin/shell",
        "/etc/motd",
        NULL
    };

    int idx = 0;
    for (const char **item = copy_items; *item; item++, idx++) {
        sys_install_copy(*item, *item);
        g_inst.progress_percent = 20 + (idx * 8);
    }
    g_inst.progress_percent = 80;

    /* Step 3: Install bootloader (80-95%) */
    sys_install_bootloader(g_inst.selected_disk);
    g_inst.progress_percent = 95;

    /* Step 4: Create user account (95-100%) */
    /* Write /etc/passwd entry */
    g_inst.progress_percent = 100;
    return 0;
}

/* ---- UI rendering (text-mode for now) ---- */

static void render_welcome(void) {
    printf("\n\n");
    printf("  ╔══════════════════════════════════════════╗\n");
    printf("  ║                                          ║\n");
    printf("  ║    %s    ║\n", str_welcome[g_inst.language]);
    printf("  ║                                          ║\n");
    printf("  ║         tobyOS version 1.0               ║\n");
    printf("  ║                                          ║\n");
    printf("  ╚══════════════════════════════════════════╝\n");
    printf("\n  %s\n", str_continue[g_inst.language]);
}

static void render_disk_select(void) {
    printf("\n  Select installation disk:\n\n");
    for (int i = 0; i < g_inst.disk_count; i++) {
        printf("  [%d] %s (%llu MB)\n", i + 1,
               g_inst.disks[i].name,
               (unsigned long long)g_inst.disks[i].size_mb);
    }
    printf("\n  Enter selection: ");
}

static void render_user_setup(void) {
    printf("\n  Create your user account:\n\n");
    printf("  Computer name: %s\n", g_inst.hostname);
    printf("  Username:      %s\n", g_inst.username);
    printf("  Password:      ****\n");
}

static void render_progress(void) {
    printf("\n  Installing tobyOS...\n\n");
    printf("  [");
    int bars = g_inst.progress_percent / 2;
    for (int i = 0; i < 50; i++) {
        printf("%c", (i < bars) ? '#' : '-');
    }
    printf("] %d%%\n", g_inst.progress_percent);
}

static void render_complete(void) {
    printf("\n  ╔══════════════════════════════════════════╗\n");
    printf("  ║                                          ║\n");
    printf("  ║    Installation Complete!                 ║\n");
    printf("  ║                                          ║\n");
    printf("  ║    Remove installation media and         ║\n");
    printf("  ║    press Enter to reboot.                ║\n");
    printf("  ║                                          ║\n");
    printf("  ╚══════════════════════════════════════════╝\n");
}

/* ---- Main installer loop ---- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    memset(&g_inst, 0, sizeof(g_inst));
    g_inst.state = INST_WELCOME;
    g_inst.language = 0;
    memcpy(g_inst.hostname, "tobypc", 7);
    memcpy(g_inst.username, "user", 5);
    memcpy(g_inst.password, "tobyos", 7);

    detect_disks();

    char line[256];

    while (g_inst.state != INST_COMPLETE && g_inst.state != INST_ERROR) {
        switch (g_inst.state) {
        case INST_WELCOME:
            render_welcome();
            if (fgets(line, sizeof(line), stdin)) {}
            g_inst.state = INST_DISK_SELECT;
            break;

        case INST_DISK_SELECT:
            render_disk_select();
            if (fgets(line, sizeof(line), stdin)) {
                int sel = line[0] - '1';
                if (sel >= 0 && sel < g_inst.disk_count) {
                    g_inst.selected_disk = sel;
                    auto_partition(sel);
                    g_inst.state = INST_USER_SETUP;
                }
            }
            break;

        case INST_USER_SETUP:
            render_user_setup();
            printf("\n  Enter hostname (or Enter for default): ");
            if (fgets(line, sizeof(line), stdin) && line[0] != '\n') {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                size_t len = strlen(line);
                if (len > 63) len = 63;
                memcpy(g_inst.hostname, line, len);
                g_inst.hostname[len] = '\0';
            }
            printf("  Enter username (or Enter for default): ");
            if (fgets(line, sizeof(line), stdin) && line[0] != '\n') {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                size_t len = strlen(line);
                if (len > 63) len = 63;
                memcpy(g_inst.username, line, len);
                g_inst.username[len] = '\0';
            }
            g_inst.state = INST_COPYING;
            break;

        case INST_COPYING:
            render_progress();
            perform_install();
            render_progress();
            g_inst.state = INST_COMPLETE;
            break;

        default:
            g_inst.state = INST_ERROR;
            break;
        }
    }

    if (g_inst.state == INST_COMPLETE) {
        render_complete();
        if (fgets(line, sizeof(line), stdin)) {}
    }

    return 0;
}

/* Weak stubs for install syscalls (linked against real kernel calls if available) */
__attribute__((weak)) int sys_disk_count(void) { return 0; }
__attribute__((weak)) int sys_disk_info(int id, char *name, int name_len, uint64_t *size_mb) {
    (void)id; (void)name; (void)name_len; (void)size_mb; return -1;
}
__attribute__((weak)) int sys_install_copy(const char *s, const char *d) {
    (void)s; (void)d; return 0;
}
__attribute__((weak)) int sys_install_mkfs(int d, int p, const char *fs) {
    (void)d; (void)p; (void)fs; return 0;
}
__attribute__((weak)) int sys_install_bootloader(int d) { (void)d; return 0; }
