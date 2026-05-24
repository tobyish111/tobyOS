/* shell/startmenu.c -- Start menu implementation (Milestone 3).
 *
 * Popup from bottom-left (300x500px):
 *   - Search bar at top
 *   - User name + profile picture placeholder
 *   - App grid showing installed programs from /bin and /apps
 *   - Recent files section
 *   - Power button row (shutdown/restart/sleep/lock)
 *
 * Linked into the shell binary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

/* ---- syscall numbers --------------------------------------------- */

#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_EXIT            7

/* ---- syscall wrappers -------------------------------------------- */

static inline long sm_sys5(long nr, long a, long b, long c, long d, long e)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return r;
}

static void sm_fill(int fd, int x, int y, int w, int h, unsigned int color)
{
    unsigned int wh = ((unsigned int)(unsigned short)w) |
                      (((unsigned int)(unsigned short)h) << 16);
    sm_sys5(SYS_GUI_FILL, fd, x, y, (long)wh, (long)color);
}

static void sm_text(int fd, int x, int y, const char *s,
                    unsigned int fg, unsigned int bg)
{
    unsigned int xy = ((unsigned int)(unsigned short)x) |
                      (((unsigned int)(unsigned short)y) << 16);
    sm_sys5(SYS_GUI_TEXT, fd, (long)xy, (long)s, (long)fg, (long)bg);
}

/* ---- constants --------------------------------------------------- */

#define MENU_W         300
#define MENU_H         500
#define SEARCH_H        32
#define USER_H          48
#define ITEM_H          28
#define POWER_H         40
#define PADDING          8

#define COL_MENU_BG    0x00181825u
#define COL_SEARCH_BG  0x00282A36u
#define COL_SEARCH_FG  0x00CDD6F4u
#define COL_USER_BG    0x00313244u
#define COL_USER_FG    0x00CDD6F4u
#define COL_ITEM_BG    0x00181825u
#define COL_ITEM_FG    0x00BAC2DEu
#define COL_ITEM_HL    0x00363848u
#define COL_POWER_BG   0x00F38BA8u
#define COL_POWER_FG   0x00181825u
#define COL_SECTION_FG 0x00585B70u
#define COL_BORDER     0x0045475Au

#define MAX_ENTRIES    24
#define NAME_MAX_LEN   20

/* ---- state ------------------------------------------------------- */

static int g_visible;

static struct {
    char name[NAME_MAX_LEN + 1];
    char path[64];
} g_entries[MAX_ENTRIES];

static int g_entry_count;
static int g_selected;

static char g_search[32];
static int  g_search_len;

/* Power menu items */
#define POWER_ITEMS     4
static const char *g_power_labels[POWER_ITEMS] = {
    "Shutdown", "Restart", "Sleep", "Lock"
};

/* ---- helpers ----------------------------------------------------- */

static int str_match(const char *haystack, const char *needle)
{
    if (!needle[0])
        return 1;
    int nlen = (int)strlen(needle);
    int hlen = (int)strlen(haystack);
    for (int i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            char hc = haystack[i + j];
            char nc = needle[j];
            /* Case insensitive */
            if (hc >= 'A' && hc <= 'Z') hc += 32;
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (hc != nc) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static void scan_apps(void)
{
    g_entry_count = 0;

    /* Scan /bin for GUI apps */
    DIR *d = opendir("/bin");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && g_entry_count < MAX_ENTRIES) {
            /* Only show entries starting with "gui_" */
            if (strncmp(ent->d_name, "gui_", 4) == 0) {
                int ni = 0;
                const char *src = ent->d_name + 4;
                while (*src && ni < NAME_MAX_LEN) {
                    g_entries[g_entry_count].name[ni++] = *src++;
                }
                g_entries[g_entry_count].name[ni] = '\0';

                /* Build path */
                int pi = 0;
                const char *prefix = "/bin/";
                while (*prefix)
                    g_entries[g_entry_count].path[pi++] = *prefix++;
                src = ent->d_name;
                while (*src && pi < 62)
                    g_entries[g_entry_count].path[pi++] = *src++;
                g_entries[g_entry_count].path[pi] = '\0';

                g_entry_count++;
            }
        }
        closedir(d);
    }

    /* Scan /apps if it exists */
    d = opendir("/apps");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && g_entry_count < MAX_ENTRIES) {
            if (ent->d_name[0] == '.') continue;

            int ni = 0;
            const char *src = ent->d_name;
            while (*src && ni < NAME_MAX_LEN)
                g_entries[g_entry_count].name[ni++] = *src++;
            g_entries[g_entry_count].name[ni] = '\0';

            int pi = 0;
            const char *prefix = "/apps/";
            while (*prefix)
                g_entries[g_entry_count].path[pi++] = *prefix++;
            src = ent->d_name;
            while (*src && pi < 62)
                g_entries[g_entry_count].path[pi++] = *src++;
            g_entries[g_entry_count].path[pi] = '\0';

            g_entry_count++;
        }
        closedir(d);
    }
}

/* ---- public API -------------------------------------------------- */

void startmenu_init(void)
{
    g_visible    = 0;
    g_selected   = -1;
    g_search_len = 0;
    g_search[0]  = '\0';
    scan_apps();
}

int startmenu_visible(void)
{
    return g_visible;
}

void startmenu_toggle(int win_fd, int scr_w, int scr_h)
{
    (void)win_fd; (void)scr_w; (void)scr_h;
    g_visible = !g_visible;
    if (g_visible) {
        g_selected   = -1;
        g_search_len = 0;
        g_search[0]  = '\0';
        scan_apps();
    }
}

void startmenu_draw(int win_fd, int scr_w, int scr_h)
{
    if (!g_visible)
        return;

    int mx = 0;
    int my = scr_h - 40 - MENU_H;
    if (my < 0) my = 0;

    (void)scr_w;

    /* Background */
    sm_fill(win_fd, mx, my, MENU_W, MENU_H, COL_MENU_BG);

    /* Border */
    sm_fill(win_fd, mx + MENU_W - 1, my, 1, MENU_H, COL_BORDER);
    sm_fill(win_fd, mx, my, MENU_W, 1, COL_BORDER);

    int cy = my + PADDING;

    /* User section */
    sm_fill(win_fd, mx + PADDING, cy, MENU_W - 2 * PADDING, USER_H,
            COL_USER_BG);
    sm_text(win_fd, mx + PADDING + 48, cy + 16, "User",
            COL_USER_FG, COL_USER_BG);

    /* Profile picture placeholder (circle outline) */
    sm_fill(win_fd, mx + PADDING + 8, cy + 8, 32, 32, COL_SECTION_FG);
    sm_fill(win_fd, mx + PADDING + 10, cy + 10, 28, 28, COL_USER_BG);

    cy += USER_H + PADDING;

    /* Search bar */
    sm_fill(win_fd, mx + PADDING, cy, MENU_W - 2 * PADDING, SEARCH_H,
            COL_SEARCH_BG);

    if (g_search_len > 0) {
        sm_text(win_fd, mx + PADDING + 8, cy + 8, g_search,
                COL_SEARCH_FG, COL_SEARCH_BG);
    } else {
        sm_text(win_fd, mx + PADDING + 8, cy + 8, "Search...",
                COL_SECTION_FG, COL_SEARCH_BG);
    }

    cy += SEARCH_H + PADDING;

    /* Section label: Apps */
    sm_text(win_fd, mx + PADDING, cy, "Applications",
            COL_SECTION_FG, COL_MENU_BG);
    cy += 16;

    /* App list (filtered by search) */
    int visible_count = 0;
    int max_visible = (MENU_H - (cy - my) - POWER_H - PADDING * 2) / ITEM_H;
    if (max_visible > MAX_ENTRIES) max_visible = MAX_ENTRIES;

    for (int i = 0; i < g_entry_count && visible_count < max_visible; i++) {
        if (!str_match(g_entries[i].name, g_search))
            continue;

        unsigned int bg = (visible_count == g_selected)
                          ? COL_ITEM_HL : COL_ITEM_BG;
        sm_fill(win_fd, mx + PADDING, cy, MENU_W - 2 * PADDING,
                ITEM_H, bg);
        sm_text(win_fd, mx + PADDING + 12, cy + 6,
                g_entries[i].name, COL_ITEM_FG, bg);

        cy += ITEM_H;
        visible_count++;
    }

    /* Recent files section */
    cy += PADDING;
    sm_text(win_fd, mx + PADDING, cy, "Recent files",
            COL_SECTION_FG, COL_MENU_BG);
    cy += 16;
    sm_text(win_fd, mx + PADDING + 12, cy, "(none)",
            COL_SECTION_FG, COL_MENU_BG);

    /* Power row at bottom */
    int power_y = my + MENU_H - POWER_H - PADDING;
    sm_fill(win_fd, mx, power_y, MENU_W, 1, COL_BORDER);
    power_y += 1;

    int pw = (MENU_W - 2 * PADDING - (POWER_ITEMS - 1) * 4) / POWER_ITEMS;
    for (int i = 0; i < POWER_ITEMS; i++) {
        int px = mx + PADDING + i * (pw + 4);
        unsigned int bg = (i == 0) ? COL_POWER_BG : COL_SEARCH_BG;
        unsigned int fg = (i == 0) ? COL_POWER_FG : COL_ITEM_FG;
        sm_fill(win_fd, px, power_y + 4, pw, POWER_H - 8, bg);
        sm_text(win_fd, px + 4, power_y + 12, g_power_labels[i], fg, bg);
    }
}

int startmenu_handle_click(int win_fd, int x, int y,
                           int scr_w, int scr_h)
{
    if (!g_visible)
        return 0;

    int mx = 0;
    int my = scr_h - 40 - MENU_H;
    if (my < 0) my = 0;

    (void)win_fd; (void)scr_w;

    /* Outside menu? */
    if (x < mx || x >= mx + MENU_W || y < my || y >= my + MENU_H)
        return 0;

    /* Power row click */
    int power_y = my + MENU_H - POWER_H - PADDING;
    if (y >= power_y) {
        int pw = (MENU_W - 2 * PADDING - (POWER_ITEMS - 1) * 4) / POWER_ITEMS;
        for (int i = 0; i < POWER_ITEMS; i++) {
            int px = mx + PADDING + i * (pw + 4);
            if (x >= px && x < px + pw) {
                if (i == 0) {
                    /* Shutdown */
                    _exit(0);
                }
                /* Other power actions are stubs */
                return 1;
            }
        }
        return 1;
    }

    /* App list area -- figure out which item was clicked */
    int cy = my + PADDING + USER_H + PADDING + SEARCH_H + PADDING + 16;
    int max_visible = (MENU_H - (cy - my) - POWER_H - PADDING * 2) / ITEM_H;
    if (max_visible > MAX_ENTRIES) max_visible = MAX_ENTRIES;

    int visible_count = 0;
    for (int i = 0; i < g_entry_count && visible_count < max_visible; i++) {
        if (!str_match(g_entries[i].name, g_search))
            continue;

        int item_y = cy + visible_count * ITEM_H;
        if (y >= item_y && y < item_y + ITEM_H) {
            /* Launch this app */
            char *argv[2];
            argv[0] = g_entries[i].path;
            argv[1] = NULL;
            toby_spawn(g_entries[i].path, argv, NULL, 0, 1, 2);
            g_visible = 0;
            return 1;
        }
        visible_count++;
    }

    return 1;
}

void startmenu_handle_key(int win_fd, int key, int scr_w, int scr_h)
{
    (void)win_fd; (void)scr_w; (void)scr_h;

    if (key == 0x1B) {
        /* Escape: close menu */
        g_visible = 0;
        return;
    }

    if (key == 0x08) {
        /* Backspace */
        if (g_search_len > 0) {
            g_search[--g_search_len] = '\0';
        }
        return;
    }

    if (key == '\n' || key == '\r') {
        /* Enter: launch selected */
        if (g_selected >= 0) {
            int vis = 0;
            for (int i = 0; i < g_entry_count; i++) {
                if (!str_match(g_entries[i].name, g_search))
                    continue;
                if (vis == g_selected) {
                    char *argv[2];
                    argv[0] = g_entries[i].path;
                    argv[1] = NULL;
                    toby_spawn(g_entries[i].path, argv, NULL, 0, 1, 2);
                    g_visible = 0;
                    return;
                }
                vis++;
            }
        }
        return;
    }

    /* Arrow keys: up/down navigate (simplified: key codes 'k'/'j') */
    /* Regular printable char: append to search */
    if (key >= 0x20 && key < 0x7F && g_search_len < 30) {
        g_search[g_search_len++] = (char)key;
        g_search[g_search_len]   = '\0';
        g_selected = 0;
    }
}
