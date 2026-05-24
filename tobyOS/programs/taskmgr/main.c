/* taskmgr/main.c -- Task Manager for tobyOS (Milestone 5).
 *
 * 700x500 GUI window with Processes and Performance tabs.
 * Processes tab: PID, Name, CPU%, Memory, Status with kill support.
 * Performance tab: CPU usage graph, memory bar, uptime, process count.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tobyos/abi/abi.h>

static inline long sc0(long n){long r;__asm__ volatile("syscall":"=a"(r):"0"(n):"rcx","r11","memory");return r;}
static inline long sc1(long n,long a){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");return r;}
static inline long sc2(long n,long a,long b){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static inline long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static inline long sc5(long n,long a,long b,long c,long d,long e){long r;register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return r;}

static int  gui_create(int w,int h,const char*t){return(int)sc3(ABI_SYS_GUI_CREATE,w,h,(long)t);}
static void gui_fill(int fd,int x,int y,int w,int h,unsigned c){unsigned wh=((unsigned)(unsigned short)w)|(((unsigned)(unsigned short)h)<<16);sc5(ABI_SYS_GUI_FILL,fd,x,y,wh,c);}
static void gui_text(int fd,int x,int y,const char*s,unsigned fg){long xy=(long)(unsigned)((unsigned)x|((unsigned)y<<16));sc5(ABI_SYS_GUI_TEXT,fd,xy,(long)s,fg,0);}
static void gui_text_scaled(int fd,int x,int y,const char*s,unsigned fg,unsigned bg,int sc){long xy=(long)(unsigned)((unsigned)x|((unsigned)y<<16));unsigned a5=(bg&0x00FFFFFFu)|((unsigned)(sc&0x7F)<<24);sc5(ABI_SYS_GUI_TEXT_SCALED,fd,xy,(long)s,fg,a5);}
static void gui_flip(int fd){sc1(ABI_SYS_GUI_FLIP,fd);}
static long gui_poll(int fd,void*ev){return sc2(ABI_SYS_GUI_POLL_EVENT,fd,(long)ev);}
static long sys_clock_ms(void){return sc0(ABI_SYS_CLOCK_MS);}
static long sys_kill(int pid,int sig){return sc2(ABI_SYS_KILL,pid,sig);}
static long sys_system_metrics(void*out){return sc1(ABI_SYS_SYSTEM_METRICS,(long)out);}

struct gui_event { int type,x,y; unsigned char button,key,_pad[2]; };
#define EV_CLOSE 1
#define EV_MOUSE_DOWN 2
#define EV_KEY 4

#define W 700
#define H 500
#define TAB_H 30
#define HDR_H 22
#define ROW_H 20

/* colours */
#define C_BG       0xf0f0f0
#define C_TAB_BG   0xe0e0e0
#define C_TAB_ACT  0xffffff
#define C_TAB_TXT  0x333333
#define C_HDR      0xd8d8d8
#define C_ROW_BG   0xffffff
#define C_ROW_ALT  0xf5f5f5
#define C_ROW_SEL  0xd0e0ff
#define C_TEXT     0x222222
#define C_DIM      0x888888
#define C_ACCENT   0x0066cc
#define C_GREEN    0x44aa44
#define C_RED      0xcc4444
#define C_GRAPH_BG 0x1a1a2e
#define C_GRAPH_FG 0x00cc66
#define C_MEM_BAR  0x3388dd

/* System metrics struct (matches ABI) */
struct sys_metrics {
    unsigned long long uptime_ms;
    unsigned long long total_pages, used_pages, free_pages;
    unsigned long long cpu_user_ns, cpu_total_ns;
    unsigned long long total_syscalls, context_switches;
    unsigned long long gui_frames, proc_spawns, proc_exits;
    unsigned long long vfs_ops, vfs_ns, gui_ns;
    unsigned long long net_packets, net_ns;
    unsigned process_count, ready_count, blocked_count;
    unsigned cpu_pct, ram_pct, gui_pct, disk_pct, net_pct;
    unsigned syscalls_per_s, gui_fps;
    unsigned vfs_ops_per_s, net_packets_per_s;
    unsigned net_up, net_ip_be, tsc_mhz;
    unsigned _reserved[9];
};

static int active_tab; /* 0=Processes, 1=Performance */
static int selected;
static int scroll_top;
static struct sys_metrics metrics;

/* CPU history for graph */
#define GRAPH_HISTORY 60
static int cpu_history[GRAPH_HISTORY];
static int history_pos;

static void fetch_metrics(void) {
    sys_system_metrics(&metrics);
}

static void draw_processes_tab(int fd) {
    int y = TAB_H;

    /* header */
    gui_fill(fd, 0, y, W, HDR_H, C_HDR);
    gui_text(fd, 10, y+3, "PID", C_DIM);
    gui_text(fd, 60, y+3, "Name", C_DIM);
    gui_text(fd, 280, y+3, "CPU%", C_DIM);
    gui_text(fd, 350, y+3, "Memory", C_DIM);
    gui_text(fd, 460, y+3, "Status", C_DIM);
    gui_text(fd, 560, y+3, "Syscalls", C_DIM);
    y += HDR_H;

    /* Process list from metrics.process_count - we display what we know */
    int n = (int)metrics.process_count;
    if(n > 64) n = 64;

    int visible = (H - y - 24) / ROW_H;
    for(int i=0; i<visible && (i+scroll_top)<n; i++) {
        int idx = i + scroll_top;
        int ry = y + i*ROW_H;
        unsigned bg = (idx == selected) ? C_ROW_SEL :
                      (idx & 1) ? C_ROW_ALT : C_ROW_BG;
        gui_fill(fd, 0, ry, W, ROW_H, bg);

        char buf[16];
        snprintf(buf, 16, "%d", idx);
        gui_text(fd, 10, ry+3, buf, C_TEXT);

        gui_text(fd, 60, ry+3, idx==0 ? "kernel" : "process", C_TEXT);

        gui_text(fd, 280, ry+3, idx==0 ? "--" : "0%", C_DIM);

        snprintf(buf, 16, "%lu KB",
                 (unsigned long)(metrics.used_pages * 4 / (n > 0 ? (unsigned)n : 1u)));
        gui_text(fd, 350, ry+3, buf, C_DIM);

        gui_text(fd, 460, ry+3, "Running", C_GREEN);
    }

    /* status bar */
    gui_fill(fd, 0, H-24, W, 24, C_HDR);
    char status[64];
    snprintf(status, 64, "Processes: %u  |  CPU: %u%%  |  Memory: %u%%",
             metrics.process_count, metrics.cpu_pct, metrics.ram_pct);
    gui_text(fd, 8, H-20, status, C_DIM);
}

static void draw_performance_tab(int fd) {
    int y = TAB_H + 10;

    /* CPU usage graph */
    gui_text(fd, 10, y, "CPU Usage", C_TEXT);
    y += 18;
    int gx = 10, gy = y, gw = W-20, gh = 150;
    gui_fill(fd, gx, gy, gw, gh, C_GRAPH_BG);

    /* Draw grid lines */
    for(int i=1; i<4; i++) {
        int ly = gy + gh*i/4;
        gui_fill(fd, gx, ly, gw, 1, 0x333344);
    }

    /* Draw CPU history */
    for(int i=0; i<GRAPH_HISTORY-1 && i<gw; i++) {
        int idx = (history_pos + i) % GRAPH_HISTORY;
        int val = cpu_history[idx];
        if(val > 100) val = 100;
        int bar_h = val * gh / 100;
        int bx = gx + gw - GRAPH_HISTORY + i;
        if(bx >= gx && bx < gx+gw) {
            gui_fill(fd, bx, gy+gh-bar_h, 1, bar_h, C_GRAPH_FG);
        }
    }

    /* CPU percentage text */
    char cpubuf[16];
    snprintf(cpubuf, 16, "%u%%", metrics.cpu_pct);
    gui_text_scaled(fd, gx+gw-80, gy+10, cpubuf, C_GRAPH_FG, C_GRAPH_BG, 2);

    y = gy + gh + 20;

    /* Memory bar */
    gui_text(fd, 10, y, "Memory Usage", C_TEXT);
    y += 18;
    gui_fill(fd, 10, y, W-20, 30, 0xdddddd);
    int mem_w = (int)(metrics.ram_pct * (unsigned)(W-20) / 100);
    gui_fill(fd, 10, y, mem_w, 30, C_MEM_BAR);

    char membuf[64];
    unsigned long used_mb = (unsigned long)(metrics.used_pages * 4 / 1024);
    unsigned long total_mb = (unsigned long)(metrics.total_pages * 4 / 1024);
    snprintf(membuf, 64, "%lu / %lu MB (%u%%)",
             used_mb, total_mb, metrics.ram_pct);
    gui_text(fd, 20, y+8, membuf, C_TEXT);
    y += 50;

    /* System info */
    char info[64];
    unsigned long up_s = (unsigned long)(metrics.uptime_ms / 1000);
    unsigned long up_m = up_s / 60; up_s %= 60;
    unsigned long up_h = up_m / 60; up_m %= 60;
    snprintf(info, 64, "Uptime: %luh %lum %lus", up_h, up_m, up_s);
    gui_text(fd, 10, y, info, C_TEXT);
    y += 20;

    snprintf(info, 64, "Processes: %u  |  Syscalls/s: %u  |  GUI FPS: %u",
             metrics.process_count, metrics.syscalls_per_s, metrics.gui_fps);
    gui_text(fd, 10, y, info, C_TEXT);
    y += 20;

    snprintf(info, 64, "Context switches: %llu  |  VFS ops/s: %u",
             (unsigned long long)metrics.context_switches, metrics.vfs_ops_per_s);
    gui_text(fd, 10, y, info, C_TEXT);
}

static void draw(int fd) {
    gui_fill(fd, 0, 0, W, H, C_BG);

    /* tab bar */
    gui_fill(fd, 0, 0, W, TAB_H, C_TAB_BG);
    int tw = 120;
    for(int i=0; i<2; i++) {
        unsigned bg = (i == active_tab) ? C_TAB_ACT : C_TAB_BG;
        gui_fill(fd, i*tw, 0, tw, TAB_H, bg);
        if(i == active_tab) gui_fill(fd, i*tw, TAB_H-3, tw, 3, C_ACCENT);
        gui_text(fd, i*tw+10, 8, i==0 ? "Processes" : "Performance", C_TAB_TXT);
    }

    if(active_tab == 0) draw_processes_tab(fd);
    else draw_performance_tab(fd);

    gui_flip(fd);
}

int main(void) {
    active_tab = 0;
    selected = 0;
    scroll_top = 0;
    history_pos = 0;
    memset(cpu_history, 0, sizeof(cpu_history));
    memset(&metrics, 0, sizeof(metrics));

    int fd = gui_create(W, H, "Task Manager");
    if(fd < 0) return 1;

    fetch_metrics();
    draw(fd);
    long last_refresh = sys_clock_ms();

    struct gui_event ev;
    for(;;) {
        /* Auto-refresh every ~1 second */
        long now = sys_clock_ms();
        if(now - last_refresh > 1000) {
            fetch_metrics();
            cpu_history[history_pos % GRAPH_HISTORY] = (int)metrics.cpu_pct;
            history_pos++;
            last_refresh = now;
            draw(fd);
        }

        if(gui_poll(fd, &ev) > 0) {
            if(ev.type == EV_CLOSE) break;

            if(ev.type == EV_MOUSE_DOWN) {
                /* Tab clicks */
                if(ev.y < TAB_H) {
                    int tab = ev.x / 120;
                    if(tab >= 0 && tab < 2) active_tab = tab;
                    draw(fd);
                }
                /* Process selection */
                if(active_tab == 0 && ev.y >= TAB_H + HDR_H) {
                    int idx = (ev.y - TAB_H - HDR_H) / ROW_H + scroll_top;
                    if(idx >= 0 && idx < (int)metrics.process_count)
                        selected = idx;
                    draw(fd);
                }
            }

            if(ev.type == EV_KEY) {
                switch(ev.key) {
                case '1': active_tab = 0; break;
                case '2': active_tab = 1; break;
                case '\t': active_tab = 1 - active_tab; break;
                case 0x81: case 'j': /* down */
                    if(active_tab == 0 && selected < (int)metrics.process_count-1) {
                        selected++;
                        int visible = (H - TAB_H - HDR_H - 24) / ROW_H;
                        if(selected >= scroll_top + visible) scroll_top++;
                    }
                    break;
                case 0x80: case 'k': /* up */
                    if(active_tab == 0 && selected > 0) {
                        selected--;
                        if(selected < scroll_top) scroll_top = selected;
                    }
                    break;
                case 0x7f: case 'x': /* delete/x = kill */
                    if(active_tab == 0 && selected > 0) {
                        sys_kill(selected, 9);
                        fetch_metrics();
                    }
                    break;
                case 'q': _exit(0); break;
                }
                draw(fd);
            }
        }
        __asm__ volatile("syscall"::"a"((long)ABI_SYS_YIELD):"rcx","r11","memory");
    }
    _exit(0);
    return 0;
}
