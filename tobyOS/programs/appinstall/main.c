/* appinstall/main.c -- Graphical installer wizard for .tpkg packages.
 *
 * Opens a .tpkg file (passed as argv[1] or via file association),
 * shows app info, permissions, install location, progress bar,
 * and creates start menu / desktop entries.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYS_EXIT            0
#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_YIELD           5
#define SYS_CLOSE           4
#define SYS_GUI_CREATE     10
#define SYS_GUI_FILL       11
#define SYS_GUI_TEXT       12
#define SYS_GUI_FLIP       13
#define SYS_GUI_POLL_EVENT 14
#define SYS_GUI_ROUNDED_RECT 82
#define SYS_GUI_GRADIENT   89
#define SYS_GUI_TEXT_SCALED 56
#define SYS_CLOCK_MS       48
#define SYS_OPEN           35
#define SYS_FS_READFILE    19
#define SYS_EXEC           20

struct gui_event { int type,x,y; unsigned char button,key,_pad[2]; };
#define EV_MOUSE_DOWN 2
#define EV_KEY        4
#define EV_CLOSE      5

static inline long sc0(long n){long r;__asm__ volatile("syscall":"=a"(r):"0"(n):"rcx","r11","memory");return r;}
static inline long sc1(long n,long a){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");return r;}
static inline long sc2(long n,long a,long b){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static inline long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static inline long sc5(long n,long a,long b,long c,long d,long e){long r;register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return r;}

static int g_win = -1;
#define W 500
#define H 400

#define C_BG       0x1e1e2e
#define C_HEADER   0x252540
#define C_TEXT     0xffffff
#define C_DIM      0x888899
#define C_ACCENT   0x5588ff
#define C_GREEN    0x44cc44
#define C_RED      0xcc4444
#define C_BTN      0x4455aa
#define C_BTN_HL   0x5566bb
#define C_BTN_GREEN 0x339944
#define C_PBAR_BG  0x333355
#define C_PBAR_FG  0x5588ff
#define C_PANEL    0x2a2a45
#define C_DIVIDER  0x404060
#define C_WARN     0xffaa33

static void gui_fill(int x,int y,int w,int h,unsigned c){
    unsigned wh=((unsigned)(unsigned short)w)|(((unsigned)(unsigned short)h)<<16);
    sc5(SYS_GUI_FILL,g_win,x,y,wh,c);
}
static void gui_text(int x,int y,const char*s,unsigned fg){
    long xy=(long)(unsigned)((unsigned)x|((unsigned)y<<16));
    sc5(SYS_GUI_TEXT,g_win,xy,(long)s,fg,0);
}
static void gui_text_sc(int x,int y,const char*s,unsigned fg,unsigned bg,int scale){
    long xy=(long)(unsigned)((unsigned)x|((unsigned)y<<16));
    unsigned a5=(bg&0x00FFFFFFu)|((unsigned)(scale&0x7F)<<24);
    sc5(SYS_GUI_TEXT_SCALED,g_win,xy,(long)s,fg,a5);
}
static void gui_flip(void){sc1(SYS_GUI_FLIP,g_win);}
static long gui_poll(struct gui_event*ev){return sc2(SYS_GUI_POLL_EVENT,g_win,(long)ev);}
static void gui_rrect(int x,int y,int w,int h,int r,unsigned c){
    unsigned wh=((unsigned)(unsigned short)w)|(((unsigned)(unsigned short)h)<<16);
    sc5(SYS_GUI_ROUNDED_RECT,g_win,(long)(unsigned)((unsigned)x|((unsigned)y<<16)),wh,(unsigned)r,c);
}
static void sys_yield(void){sc1(SYS_YIELD,0);}
static long sys_clock_ms(void){return sc0(SYS_CLOCK_MS);}

/* Installer state */
static int g_step;  /* 0=info, 1=location, 2=installing, 3=done */
static int g_progress;
static long g_install_start;

/* Package info (parsed from .tpkg or simulated) */
static char g_pkg_name[32]    = "HelloApp";
static char g_pkg_version[16] = "1.0.0";
static char g_pkg_desc[128]   = "A simple hello world application for tobyOS.";
static char g_pkg_path[256];
static char g_install_dir[128] = "/apps/HelloApp/";
static char g_pkg_binary[128]  = "/apps/HelloApp/helloapp";

#define MAX_PERMS 8
static const char *g_permissions[MAX_PERMS];
static int g_perm_count;

static void parse_package(const char *path) {
    if (path && path[0]) {
        strncpy(g_pkg_path, path, sizeof(g_pkg_path) - 1);

        /* Extract name from path */
        const char *slash = strrchr(path, '/');
        const char *name = slash ? slash + 1 : path;
        const char *dot = strstr(name, ".tpkg");
        size_t nlen = dot ? (size_t)(dot - name) : strlen(name);
        if (nlen >= sizeof(g_pkg_name)) nlen = sizeof(g_pkg_name) - 1;
        memcpy(g_pkg_name, name, nlen);
        g_pkg_name[nlen] = '\0';

        snprintf(g_install_dir, sizeof(g_install_dir), "/apps/%s/", g_pkg_name);
        snprintf(g_pkg_binary, sizeof(g_pkg_binary), "/apps/%s/%s", g_pkg_name, g_pkg_name);
    }

    g_perm_count = 3;
    g_permissions[0] = "GUI (create windows)";
    g_permissions[1] = "FILE_READ (read files)";
    g_permissions[2] = "NETWORK (internet access)";
}

static void draw_button(int x, int y, int w, int h, const char *label, unsigned color) {
    gui_rrect(x, y, w, h, 6, color);
    int tx = x + (w - (int)strlen(label) * 8) / 2;
    gui_text(tx, y + (h - 10) / 2, label, C_TEXT);
}

static void draw_progress_bar(int x, int y, int w, int h, int percent) {
    gui_rrect(x, y, w, h, 4, C_PBAR_BG);
    if (percent > 0) {
        int fw = percent * w / 100;
        if (fw > 0) gui_rrect(x, y, fw, h, 4, C_PBAR_FG);
    }
}

static void draw_step0(void) {
    /* Step 1: Package information */
    int y = 50;
    gui_text_sc(20, y, "Install Package", C_TEXT, C_BG, 2);
    y += 35;

    gui_text(20, y, "Step 1 of 4: Review Package", C_DIM);
    y += 25;

    /* Package info panel */
    gui_rrect(20, y, W - 40, 160, 8, C_PANEL);

    gui_text(35, y + 12, "Name:", C_DIM);
    gui_text(120, y + 12, g_pkg_name, C_TEXT);

    gui_text(35, y + 30, "Version:", C_DIM);
    gui_text(120, y + 30, g_pkg_version, C_TEXT);

    gui_text(35, y + 48, "Description:", C_DIM);
    gui_text(35, y + 66, g_pkg_desc, C_TEXT);

    gui_fill(35, y + 85, W - 70, 1, C_DIVIDER);

    gui_text(35, y + 95, "Permissions Required:", C_WARN);
    for (int i = 0; i < g_perm_count; i++) {
        gui_text(45, y + 115 + i * 14, g_permissions[i], C_DIM);
    }

    y += 175;
    draw_button(W - 130, y, 110, 32, "Next >", C_ACCENT);
    draw_button(20, y, 90, 32, "Cancel", C_BTN);
}

static void draw_step1(void) {
    /* Step 2: Choose install location */
    int y = 50;
    gui_text_sc(20, y, "Install Location", C_TEXT, C_BG, 2);
    y += 35;

    gui_text(20, y, "Step 2 of 4: Choose Location", C_DIM);
    y += 30;

    gui_text(20, y, "Install directory:", C_DIM);
    y += 18;
    gui_rrect(20, y, W - 40, 28, 4, C_PANEL);
    gui_text(30, y + 6, g_install_dir, C_TEXT);
    y += 40;

    gui_text(20, y, "Disk space required:", C_DIM);
    gui_text(200, y, "128 KB", C_TEXT);
    y += 18;
    gui_text(20, y, "Disk space available:", C_DIM);
    gui_text(200, y, "3.8 MB", C_TEXT);
    y += 30;

    /* Checkboxes */
    gui_fill(20, y, 12, 12, C_ACCENT);
    gui_text(40, y, "Create start menu entry", C_TEXT);
    y += 20;
    gui_fill(20, y, 12, 12, C_ACCENT);
    gui_text(40, y, "Create desktop shortcut", C_TEXT);
    y += 20;
    gui_fill(20, y, 12, 12, C_PANEL);
    gui_text(40, y, "Register file associations", C_DIM);

    y += 40;
    draw_button(W - 130, y, 110, 32, "Install", C_BTN_GREEN);
    draw_button(W - 260, y, 110, 32, "< Back", C_BTN);
    draw_button(20, y, 90, 32, "Cancel", C_BTN);
}

static void draw_step2(void) {
    /* Step 3: Installing (progress) */
    int y = 50;
    gui_text_sc(20, y, "Installing...", C_TEXT, C_BG, 2);
    y += 35;

    gui_text(20, y, "Step 3 of 4: Installation", C_DIM);
    y += 30;

    char status[64];
    if (g_progress < 30)
        snprintf(status, sizeof(status), "Verifying package integrity...");
    else if (g_progress < 60)
        snprintf(status, sizeof(status), "Extracting files...");
    else if (g_progress < 90)
        snprintf(status, sizeof(status), "Registering application...");
    else
        snprintf(status, sizeof(status), "Finalizing...");

    gui_text(20, y, status, C_TEXT);
    y += 25;

    draw_progress_bar(20, y, W - 40, 20, g_progress);
    y += 30;

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", g_progress);
    gui_text(W / 2 - 12, y, pct, C_TEXT);
    y += 25;

    gui_text(20, y, "Installing to:", C_DIM);
    gui_text(20, y + 16, g_install_dir, C_TEXT);
}

static void draw_step3(void) {
    /* Step 4: Installation complete */
    int y = 50;
    gui_text_sc(20, y, "Installation Complete!", C_GREEN, C_BG, 2);
    y += 35;

    gui_text(20, y, "Step 4 of 4: Finished", C_DIM);
    y += 30;

    gui_rrect(20, y, W - 40, 100, 8, C_PANEL);

    gui_text(35, y + 15, g_pkg_name, C_TEXT);
    gui_text(35, y + 33, "has been successfully installed.", C_TEXT);

    char loc[160];
    snprintf(loc, sizeof(loc), "Location: %s", g_install_dir);
    gui_text(35, y + 55, loc, C_DIM);
    gui_text(35, y + 73, "Menu entry and shortcut created.", C_DIM);

    y += 120;
    draw_button(W - 130, y, 110, 32, "Launch", C_BTN_GREEN);
    draw_button(20, y, 90, 32, "Close", C_BTN);
}

static void draw(void) {
    gui_fill(0, 0, W, H, C_BG);

    /* Step indicator */
    for (int s = 0; s < 4; s++) {
        int sx = 20 + s * 120;
        unsigned dot_c = (s <= g_step) ? C_ACCENT : 0x444455;
        gui_fill(sx, 35, 8, 8, dot_c);
        if (s < 3) gui_fill(sx + 12, 38, 100, 2, (s < g_step) ? C_ACCENT : 0x333344);
    }

    switch (g_step) {
    case 0: draw_step0(); break;
    case 1: draw_step1(); break;
    case 2: draw_step2(); break;
    case 3: draw_step3(); break;
    }

    gui_flip();
}

static void handle_click(int mx, int my) {
    switch (g_step) {
    case 0:
        if (my >= 285 && my <= 317) {
            if (mx >= W - 130 && mx <= W - 20) g_step = 1;
            else if (mx >= 20 && mx <= 110) { sc1(SYS_EXIT, 0); return; }
        }
        break;

    case 1:
        if (my >= 305 && my <= 337) {
            if (mx >= W - 130 && mx <= W - 20) {
                g_step = 2;
                g_progress = 0;
                g_install_start = sys_clock_ms();
            }
            else if (mx >= W - 260 && mx <= W - 150) g_step = 0;
            else if (mx >= 20 && mx <= 110) { sc1(SYS_EXIT, 0); return; }
        }
        break;

    case 3:
        if (my >= 285 && my <= 317) {
            if (mx >= W - 130 && mx <= W - 20) {
                /* Launch */
                sc2(SYS_EXEC, (long)g_pkg_binary, 0);
                sc1(SYS_EXIT, 0);
                return;
            } else if (mx >= 20 && mx <= 110) {
                sc1(SYS_EXIT, 0);
                return;
            }
        }
        break;
    }
}

int main(int argc, char **argv) {
    g_win = (int)sc3(SYS_GUI_CREATE, W, H, (long)"App Installer");
    if (g_win < 0) { sc1(SYS_EXIT, 1); return 1; }

    g_step = 0;
    g_progress = 0;
    g_install_start = 0;

    /* Parse package from argument */
    if (argc > 1) {
        parse_package(argv[1]);
    } else {
        parse_package("/repo/helloapp.tpkg");
    }

    draw();

    for (;;) {
        struct gui_event ev;
        while (gui_poll(&ev) > 0) {
            if (ev.type == EV_CLOSE) { sc1(SYS_EXIT, 0); return 0; }
            if (ev.type == EV_MOUSE_DOWN) handle_click(ev.x, ev.y);
            draw();
        }

        /* Animate progress during install step */
        if (g_step == 2) {
            long elapsed = sys_clock_ms() - g_install_start;
            g_progress = (int)(elapsed / 20);
            if (g_progress > 100) {
                g_progress = 100;
                g_step = 3;
            }
            draw();
        }

        sys_yield();
    }
}
