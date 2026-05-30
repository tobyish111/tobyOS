/* files/main.c -- File Manager for tobyOS (Milestone 5).
 *
 * 900x600 GUI window with sidebar, breadcrumb bar, file list, toolbar,
 * status bar. Supports navigation, new folder, delete, rename.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tobyos/abi/abi.h>

/* ---- syscall helpers ---- */
static inline long sc0(long n){long r;__asm__ volatile("syscall":"=a"(r):"0"(n):"rcx","r11","memory");return r;}
static inline long sc1(long n,long a){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");return r;}
static inline long sc2(long n,long a,long b){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static inline long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static inline long sc5(long n,long a,long b,long c,long d,long e){long r;register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return r;}

static int  gui_create(int w,int h,const char*t){return(int)sc3(ABI_SYS_GUI_CREATE,w,h,(long)t);}
static void gui_fill(int fd,int x,int y,int w,int h,unsigned c){unsigned wh=((unsigned)(unsigned short)w)|(((unsigned)(unsigned short)h)<<16);sc5(ABI_SYS_GUI_FILL,fd,x,y,wh,c);}
static void gui_text(int fd,int x,int y,const char*s,unsigned fg){long xy=(long)(unsigned)((unsigned)x|((unsigned)y<<16));sc5(ABI_SYS_GUI_TEXT,fd,xy,(long)s,fg,0);}
static void gui_flip(int fd){sc1(ABI_SYS_GUI_FLIP,fd);}
static long gui_poll(int fd,void*ev){return sc2(ABI_SYS_GUI_POLL_EVENT,fd,(long)ev);}
static void gui_set_title(int fd,const char*t){sc2(ABI_SYS_GUI_SET_TITLE,fd,(long)t);}
static long sys_readdir(const char*p,void*buf,int cap){return sc3(ABI_SYS_FS_READDIR,(long)p,(long)buf,cap);}
static void sys_exec(const char*p){sc1(ABI_SYS_EXEC,(long)p);}
static long sys_mkdir(const char*p,int m){return sc2(ABI_SYS_MKDIR,(long)p,m);}
static long sys_unlink(const char*p){return sc1(ABI_SYS_UNLINK,(long)p);}

struct gui_event { int type,x,y; unsigned char button,key,_pad[2]; };
#define EV_CLOSE 5  /* match kernel GUI_EV_CLOSE; type 1 is MOUSE_MOVE */
#define EV_MOUSE_DOWN 2
#define EV_KEY 4

#define WIN_W 900
#define WIN_H 600
#define SIDEBAR_W 180
#define TOOLBAR_H 32
#define STATUSBAR_H 24
#define BREADCRUMB_H 28
#define ROW_H 22
#define NAME_MAX_D 64

/* colours */
#define C_BG       0xf0f0f0
#define C_SIDEBAR  0xe0e4ea
#define C_SIDEBAR_SEL 0xd0d8e8
#define C_TOOLBAR  0xffffff
#define C_LIST_BG  0xffffff
#define C_LIST_SEL 0xcce0ff
#define C_LIST_ALT 0xf8f8f8
#define C_TEXT     0x222222
#define C_DIM      0x888888
#define C_ACCENT   0x0066cc
#define C_STATUS   0xe8e8e8
#define C_BTN      0xdddddd
#define C_BTN_TXT  0x333333
#define C_DIR_ICON 0xddaa33
#define C_FILE_ICON 0x6699cc

struct dir_entry {
    char     name[NAME_MAX_D];
    unsigned type; /* 1=file,2=dir */
    unsigned size;
};

static char cwd[256];
static struct dir_entry entries[128];
static int entry_count;
static int selected;
static int scroll_top;
static char history[16][256];
static int hist_pos, hist_len;

/* sidebar items */
static const char *sidebar_items[] = {
    "Home", "Documents", "Downloads", "Trash", "Root", NULL
};
static const char *sidebar_paths[] = {
    "/home", "/home/Documents", "/home/Downloads", "/home/Trash", "/", NULL
};

static void str_copy(char *d, const char *s, int cap) {
    int i=0; for(;s[i]&&i<cap-1;i++) d[i]=s[i]; d[i]='\0';
}

static void path_join(char *out, const char *dir, const char *name) {
    int i=0;
    for(;dir[i]&&i<250;i++) out[i]=dir[i];
    if(i>0 && out[i-1]!='/') out[i++]='/';
    int j=0;
    for(;name[j]&&i<254;j++,i++) out[i]=name[j];
    out[i]='\0';
}

static void path_up(char *path) {
    int len=(int)strlen(path);
    if(len<=1){path[0]='/';path[1]='\0';return;}
    if(path[len-1]=='/') len--;
    while(len>0 && path[len-1]!='/') len--;
    if(len==0) len=1;
    path[len]='\0';
}

static void navigate(const char *path) {
    str_copy(cwd, path, 256);
    if(cwd[0]=='\0'){cwd[0]='/';cwd[1]='\0';}

    entry_count = (int)sys_readdir(cwd, entries, 128);
    if(entry_count < 0) entry_count = 0;

    selected = 0;
    scroll_top = 0;

    /* push history */
    if(hist_pos < 15) {
        str_copy(history[hist_pos], cwd, 256);
        hist_pos++;
        hist_len = hist_pos;
    }
}

static void format_size(unsigned sz, char *buf) {
    if(sz < 1024) { snprintf(buf, 16, "%u B", sz); return; }
    if(sz < 1024*1024) { snprintf(buf, 16, "%u KB", sz/1024); return; }
    snprintf(buf, 16, "%u MB", sz/(1024*1024));
}

static void draw(int fd) {
    /* toolbar */
    gui_fill(fd, 0, 0, WIN_W, TOOLBAR_H, C_TOOLBAR);
    int bx = SIDEBAR_W + 8;
    /* Back button */
    gui_fill(fd, bx, 4, 50, 24, C_BTN);
    gui_text(fd, bx+10, 8, "Back", C_BTN_TXT);
    bx += 56;
    /* Forward */
    gui_fill(fd, bx, 4, 50, 24, C_BTN);
    gui_text(fd, bx+6, 8, "Fwd", C_BTN_TXT);
    bx += 56;
    /* Up */
    gui_fill(fd, bx, 4, 40, 24, C_BTN);
    gui_text(fd, bx+10, 8, "Up", C_BTN_TXT);
    bx += 46;
    /* New Folder */
    gui_fill(fd, bx, 4, 76, 24, C_BTN);
    gui_text(fd, bx+4, 8, "New Dir", C_BTN_TXT);
    bx += 82;
    /* Delete */
    gui_fill(fd, bx, 4, 56, 24, C_BTN);
    gui_text(fd, bx+4, 8, "Delete", C_BTN_TXT);

    /* breadcrumb */
    int cy = TOOLBAR_H;
    gui_fill(fd, SIDEBAR_W, cy, WIN_W-SIDEBAR_W, BREADCRUMB_H, 0xf8f8ff);
    gui_text(fd, SIDEBAR_W+8, cy+7, cwd, C_ACCENT);

    /* sidebar */
    gui_fill(fd, 0, 0, SIDEBAR_W, WIN_H, C_SIDEBAR);
    for(int i=0; sidebar_items[i]; i++) {
        int sy = 40 + i*28;
        unsigned bg = C_SIDEBAR;
        if(strcmp(cwd, sidebar_paths[i])==0) bg = C_SIDEBAR_SEL;
        gui_fill(fd, 0, sy, SIDEBAR_W, 26, bg);
        gui_text(fd, 16, sy+6, sidebar_items[i], C_TEXT);
    }

    /* file list area */
    int ly = TOOLBAR_H + BREADCRUMB_H;
    int lh = WIN_H - ly - STATUSBAR_H;
    int lx = SIDEBAR_W;
    int lw = WIN_W - SIDEBAR_W;
    gui_fill(fd, lx, ly, lw, lh, C_LIST_BG);

    /* header */
    gui_fill(fd, lx, ly, lw, ROW_H, C_STATUS);
    gui_text(fd, lx+30, ly+4, "Name", C_DIM);
    gui_text(fd, lx+lw-200, ly+4, "Size", C_DIM);
    gui_text(fd, lx+lw-100, ly+4, "Type", C_DIM);
    ly += ROW_H;
    lh -= ROW_H;

    int visible = lh / ROW_H;
    for(int i=0; i<visible && (i+scroll_top)<entry_count; i++) {
        int idx = i + scroll_top;
        struct dir_entry *e = &entries[idx];
        int ry = ly + i*ROW_H;
        unsigned bg = (idx == selected) ? C_LIST_SEL :
                      (idx & 1) ? C_LIST_ALT : C_LIST_BG;
        gui_fill(fd, lx, ry, lw, ROW_H, bg);

        /* icon indicator */
        unsigned icon_c = (e->type == 2) ? C_DIR_ICON : C_FILE_ICON;
        gui_fill(fd, lx+8, ry+4, 14, 14, icon_c);

        gui_text(fd, lx+30, ry+4, e->name, C_TEXT);

        if(e->type == 1) {
            char sbuf[16];
            format_size(e->size, sbuf);
            gui_text(fd, lx+lw-200, ry+4, sbuf, C_DIM);
        }
        gui_text(fd, lx+lw-100, ry+4, e->type==2?"Folder":"File", C_DIM);
    }

    /* status bar */
    gui_fill(fd, 0, WIN_H-STATUSBAR_H, WIN_W, STATUSBAR_H, C_STATUS);
    char status[64];
    snprintf(status, 64, "%d items", entry_count);
    gui_text(fd, SIDEBAR_W+8, WIN_H-STATUSBAR_H+5, status, C_DIM);

    gui_flip(fd);
}

static void open_selected(void) {
    if(selected < 0 || selected >= entry_count) return;
    struct dir_entry *e = &entries[selected];
    if(e->type == 2) {
        char path[256];
        path_join(path, cwd, e->name);
        navigate(path);
    } else {
        /* try to exec it or open with viewer */
        char path[256];
        path_join(path, cwd, e->name);
        sys_exec(path);
    }
}

int main(void) {
    str_copy(cwd, "/", 256);
    hist_pos = 0;
    hist_len = 0;
    selected = 0;
    scroll_top = 0;

    int fd = gui_create(WIN_W, WIN_H, "Files");
    if(fd < 0) return 1;

    navigate("/");
    draw(fd);

    struct gui_event ev;
    for(;;) {
        if(gui_poll(fd, &ev) > 0) {
            if(ev.type == EV_CLOSE) break;

            if(ev.type == EV_MOUSE_DOWN) {
                /* Sidebar clicks */
                if(ev.x < SIDEBAR_W) {
                    for(int i=0; sidebar_items[i]; i++) {
                        int sy = 40 + i*28;
                        if(ev.y >= sy && ev.y < sy+26) {
                            navigate(sidebar_paths[i]);
                            draw(fd); break;
                        }
                    }
                }
                /* Toolbar clicks */
                else if(ev.y < TOOLBAR_H) {
                    int bx = SIDEBAR_W + 8;
                    if(ev.x >= bx && ev.x < bx+50) { /* Back */
                        if(hist_pos > 1) { hist_pos--; navigate(history[hist_pos-1]); hist_pos--; }
                    }
                    bx += 56;
                    if(ev.x >= bx && ev.x < bx+50) { /* Fwd */
                        if(hist_pos < hist_len) { navigate(history[hist_pos]); }
                    }
                    bx += 56;
                    if(ev.x >= bx && ev.x < bx+40) { /* Up */
                        path_up(cwd); navigate(cwd);
                    }
                    bx += 46;
                    if(ev.x >= bx && ev.x < bx+76) { /* New Folder */
                        char path[256];
                        path_join(path, cwd, "NewFolder");
                        sys_mkdir(path, 0755);
                        navigate(cwd);
                    }
                    bx += 82;
                    if(ev.x >= bx && ev.x < bx+56) { /* Delete */
                        if(selected >= 0 && selected < entry_count) {
                            char path[256];
                            path_join(path, cwd, entries[selected].name);
                            sys_unlink(path);
                            navigate(cwd);
                        }
                    }
                    draw(fd);
                }
                /* File list clicks */
                else {
                    int ly = TOOLBAR_H + BREADCRUMB_H + ROW_H;
                    if(ev.y >= ly && ev.x >= SIDEBAR_W) {
                        int idx = (ev.y - ly) / ROW_H + scroll_top;
                        if(idx >= 0 && idx < entry_count) {
                            if(idx == selected) {
                                open_selected();
                            } else {
                                selected = idx;
                            }
                            draw(fd);
                        }
                    }
                }
            }

            if(ev.type == EV_KEY) {
                unsigned char k = ev.key;
                int lh = WIN_H - TOOLBAR_H - BREADCRUMB_H - STATUSBAR_H - ROW_H;
                int visible = lh / ROW_H;
                switch(k) {
                case 0x81: /* down */
                case 'j':
                    if(selected < entry_count-1) {
                        selected++;
                        if(selected >= scroll_top+visible) scroll_top++;
                    }
                    draw(fd); break;
                case 0x80: /* up */
                case 'k':
                    if(selected > 0) {
                        selected--;
                        if(selected < scroll_top) scroll_top = selected;
                    }
                    draw(fd); break;
                case '\n': case '\r':
                    open_selected(); draw(fd); break;
                case 0x08: /* backspace = up dir */
                    path_up(cwd); navigate(cwd); draw(fd); break;
                case 0x7f: /* del = delete */
                    if(selected >= 0 && selected < entry_count) {
                        char path[256];
                        path_join(path, cwd, entries[selected].name);
                        sys_unlink(path);
                        navigate(cwd); draw(fd);
                    }
                    break;
                case 'n': { /* new folder */
                    char path[256];
                    path_join(path, cwd, "NewFolder");
                    sys_mkdir(path, 0755);
                    navigate(cwd); draw(fd);
                    break;
                }
                case 'q': _exit(0); break;
                case 'r': navigate(cwd); draw(fd); break;
                }
            }
        }
        __asm__ volatile("syscall"::"a"((long)ABI_SYS_YIELD):"rcx","r11","memory");
    }
    _exit(0);
    return 0;
}
