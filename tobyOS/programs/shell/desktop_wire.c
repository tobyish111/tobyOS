/* desktop_wire.c -- Desktop integration layer for the tobyOS shell.
 *
 * Provides real system integration: app enumeration, app launching,
 * window list queries, system tray status, and search functionality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ---- Syscall numbers for system queries ---- */

#define SYS_NANOSLEEP       47
#define SYS_GUI_WIN_LIST    60
#define SYS_BAT_STATUS      70
#define SYS_NET_STATUS      71
#define SYS_AUDIO_VOLUME    72
#define SYS_TIME_GET        73

/* ---- Types ---- */

#define APP_MAX        64
#define APP_NAME_MAX   32
#define APP_PATH_MAX   64

struct desktop_app {
    char name[APP_NAME_MAX];
    char path[APP_PATH_MAX];
    int  is_gui;
};

static struct desktop_app g_apps[APP_MAX];
static int g_app_count;

#define WINDOW_MAX  32

struct window_info {
    int   id;
    int   pid;
    char  title[32];
    int   x, y, w, h;
    int   visible;
};

struct systray_status {
    int  battery_percent;
    int  battery_charging;
    int  network_connected;
    int  volume_percent;
    int  hour;
    int  minute;
};

/* ---- Spawn helper (from main.c) ---- */

extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

/* ---- Syscall wrappers ---- */

static inline long _sc1(long nr, long a)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a)
        : "rcx", "r11", "memory");
    return r;
}

static inline long _sc2(long nr, long a, long b)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a), "S"(b)
        : "rcx", "r11", "memory");
    return r;
}

__attribute__((unused))
static inline long _sc3(long nr, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return r;
}

/* ---- App enumeration ---- */

static int is_executable(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return (st.st_mode & 0111) != 0;
}

static void scan_directory(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_app_count < APP_MAX) {
        if (ent->d_name[0] == '.') continue;

        struct desktop_app *app = &g_apps[g_app_count];
        memset(app, 0, sizeof(*app));

        /* Build full path */
        size_t dlen = strlen(dir);
        size_t nlen = strlen(ent->d_name);
        if (dlen + 1 + nlen >= APP_PATH_MAX) continue;

        memcpy(app->path, dir, dlen);
        app->path[dlen] = '/';
        memcpy(app->path + dlen + 1, ent->d_name, nlen);
        app->path[dlen + 1 + nlen] = '\0';

        if (!is_executable(app->path)) continue;

        /* Extract name (strip prefix like "gui_") */
        const char *name = ent->d_name;
        if (strncmp(name, "gui_", 4) == 0) {
            name += 4;
            app->is_gui = 1;
        }

        size_t copy_len = strlen(name);
        if (copy_len >= APP_NAME_MAX) copy_len = APP_NAME_MAX - 1;
        memcpy(app->name, name, copy_len);

        g_app_count++;
    }

    closedir(d);
}

int desktop_enumerate_apps(struct desktop_app *out, int max)
{
    g_app_count = 0;
    scan_directory("/bin");
    scan_directory("/apps");

    int n = g_app_count < max ? g_app_count : max;
    if (out) memcpy(out, g_apps, (size_t)n * sizeof(struct desktop_app));
    return n;
}

/* ---- App launching ---- */

pid_t desktop_launch_app(const char *path)
{
    if (!path || !path[0]) return -1;
    char *argv[2];
    argv[0] = (char *)path;
    argv[1] = NULL;
    return toby_spawn(path, argv, NULL, 0, 1, 2);
}

/* ---- Window list (taskbar integration) ---- */

static struct window_info g_windows[WINDOW_MAX];
static int g_window_count;

int taskbar_update_windows(struct window_info *out, int max)
{
    /* Query compositor for active window list via syscall */
    long n = _sc2(SYS_GUI_WIN_LIST, (long)g_windows,
                  (long)WINDOW_MAX);
    if (n < 0) n = 0;
    g_window_count = (int)n;

    int count = g_window_count < max ? g_window_count : max;
    if (out) memcpy(out, g_windows, (size_t)count * sizeof(struct window_info));
    return count;
}

/* ---- System tray ---- */

void systray_update(struct systray_status *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));

    /* Battery */
    long bat = _sc1(SYS_BAT_STATUS, 0);
    if (bat >= 0) {
        status->battery_percent = (int)(bat & 0xFF);
        status->battery_charging = (int)((bat >> 8) & 1);
    } else {
        status->battery_percent = -1;
    }

    /* Network */
    long net = _sc1(SYS_NET_STATUS, 0);
    status->network_connected = (net > 0) ? 1 : 0;

    /* Volume */
    long vol = _sc1(SYS_AUDIO_VOLUME, 0);
    status->volume_percent = (vol >= 0) ? (int)vol : 50;

    /* Time */
    long t = _sc1(SYS_TIME_GET, 0);
    if (t >= 0) {
        status->hour = (int)((t >> 8) & 0xFF);
        status->minute = (int)(t & 0xFF);
    }
}

/* ---- Search (indexer pipe query) ---- */

#define SEARCH_RESULT_MAX  16
#define SEARCH_LINE_MAX    128

struct search_result {
    char path[APP_PATH_MAX];
    char name[APP_NAME_MAX];
};

static struct search_result g_results[SEARCH_RESULT_MAX];

int search_query(const char *text, struct search_result *out, int max)
{
    if (!text || !text[0]) return 0;

    /* Try to connect to indexer via pipe at /tmp/indexer.pipe */
    int fd = open("/tmp/indexer.pipe", O_WRONLY);
    if (fd < 0) {
        /* Fallback: simple prefix match against known apps */
        int count = 0;
        for (int i = 0; i < g_app_count && count < max; i++) {
            const char *name = g_apps[i].name;
            const char *q = text;
            int match = 1;
            while (*q) {
                if (*q != *name && (*q | 0x20) != (*name | 0x20)) {
                    match = 0;
                    break;
                }
                q++;
                name++;
            }
            if (match) {
                memcpy(g_results[count].path, g_apps[i].path, APP_PATH_MAX);
                memcpy(g_results[count].name, g_apps[i].name, APP_NAME_MAX);
                count++;
            }
        }
        if (out && count > 0) {
            int n = count < max ? count : max;
            memcpy(out, g_results, (size_t)n * sizeof(struct search_result));
        }
        return count;
    }

    /* Send query to indexer */
    write(fd, text, strlen(text));
    write(fd, "\n", 1);
    close(fd);

    /* Read results from response pipe */
    int rfd = open("/tmp/indexer_reply.pipe", O_RDONLY);
    if (rfd < 0) return 0;

    char buf[2048];
    ssize_t n = read(rfd, buf, sizeof(buf) - 1);
    close(rfd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    /* Parse results (one path per line) */
    int count = 0;
    char *line = buf;
    while (*line && count < max && count < SEARCH_RESULT_MAX) {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        if (*line) {
            size_t llen = strlen(line);
            if (llen >= APP_PATH_MAX) llen = APP_PATH_MAX - 1;
            memcpy(g_results[count].path, line, llen);
            g_results[count].path[llen] = '\0';

            /* Extract filename as name */
            const char *slash = line + llen;
            while (slash > line && *(slash - 1) != '/') slash--;
            size_t nlen = strlen(slash);
            if (nlen >= APP_NAME_MAX) nlen = APP_NAME_MAX - 1;
            memcpy(g_results[count].name, slash, nlen);
            g_results[count].name[nlen] = '\0';
            count++;
        }

        if (saved == '\0') break;
        line = eol + 1;
    }

    if (out && count > 0) {
        int ret = count < max ? count : max;
        memcpy(out, g_results, (size_t)ret * sizeof(struct search_result));
    }
    return count;
}
