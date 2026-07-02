/* user_gui_taskmgr/main.c -- /bin/gui_taskmgr, a tabbed GUI task manager.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The data model is unchanged from
 * the original raw-gui_* version: live metrics via SYS_SYSTEM_METRICS and the
 * service/process list via SYS_SVC_LIST, refreshed ~1s. The UI is now
 * toolkit-native: a button tab-bar (Processes / Performance / Services), the
 * two list tabs are TK_TABLEs sourced from the live service array, and the
 * Performance tab is a TK_CANVAS that custom-draws the bars / history graphs /
 * counters. The app drives its own loop (tk_pump + paced refresh) so the tables
 * and graphs tick live; tab content is swapped with tk_checkpoint / tk_rewind.
 */

#include <toby/tk.h>
#include <unistd.h>

#define SYS_CLOCK_MS       48
#define SYS_SVC_LIST       65
#define SYS_HWINFO         67
#define SYS_SYSTEM_METRICS 74
#define SYS_KILL           75

#define ABI_SVC_NAME_MAX 16
#define ABI_SVC_PATH_MAX 64

struct abi_service_info {
    char     name[ABI_SVC_NAME_MAX];
    char     path[ABI_SVC_PATH_MAX];
    uint32_t state;
    uint32_t kind;
    int32_t  pid;
    int32_t  last_exit;
    uint32_t restart_count;
    uint32_t crash_count;
    uint64_t last_start_ms;
    uint64_t last_crash_ms;
    uint64_t backoff_until_ms;
    unsigned char autorestart;
    unsigned char _pad[7];
    uint64_t _reserved[2];
};

struct abi_system_metrics {
    uint64_t uptime_ms, total_pages, used_pages, free_pages;
    uint64_t cpu_user_ns, cpu_total_ns, total_syscalls, context_switches;
    uint64_t gui_frames, proc_spawns, proc_exits, vfs_ops, vfs_ns, gui_ns;
    uint64_t net_packets, net_ns;
    uint32_t process_count, ready_count, blocked_count, cpu_pct, ram_pct, gui_pct;
    uint32_t disk_pct, net_pct, syscalls_per_s, gui_fps, vfs_ops_per_s;
    uint32_t net_packets_per_s, net_up, net_ip_be, tsc_mhz, _reserved[9];
};

/* Mirror of the kernel-frozen struct abi_hwinfo_summary (SYS_HWINFO, 192 B).
 * Static hardware identity read once from CPUID/SMP -- lets the monitor name
 * the actual CPU + core count of the machine it booted on, not a guess. */
struct abi_hwinfo_summary {
    char     cpu_vendor[16];       /* "GenuineIntel"                     */
    char     cpu_brand[48];        /* "Intel(R) Core(TM) i5-4590 ..."    */
    uint32_t cpu_count;            /* logical CPUs detected (all cores)  */
    uint32_t cpu_family, cpu_model, cpu_stepping, cpu_features, _pad0;
    uint64_t mem_total_pages, mem_used_pages, mem_free_pages;
    uint16_t pci_count, usb_count, blk_count, input_count;
    uint16_t audio_count, battery_count, hub_count, display_count;
    uint64_t boot_uptime_ms, snapshot_epoch;
    uint32_t kernel_abi_ver, safe_mode;
    char     profile_hint[16];
    uint64_t _reserved[3];
};

/* ---- syscall stubs (data only; drawing is via TobyTK) ---------------- */
static inline long sc0(long n){long r;__asm__ volatile("syscall":"=a"(r):"0"(n):"rcx","r11","memory");return r;}
static inline long sc1(long n,long a){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");return r;}
static inline long sc2(long n,long a,long b){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static long sys_clock_ms(void){ return sc0(SYS_CLOCK_MS); }
static long sys_svc_list(struct abi_service_info *o,uint32_t cap){ return sc2(SYS_SVC_LIST,(long)o,(long)cap); }
static long sys_system_metrics(struct abi_system_metrics *o){ return sc1(SYS_SYSTEM_METRICS,(long)o); }
static long sys_hwinfo(struct abi_hwinfo_summary *o){ return sc1(SYS_HWINFO,(long)o); }
static long sys_kill(int pid){ return sc1(SYS_KILL,(long)pid); }

/* ---- tiny helpers ---------------------------------------------------- */
static void my_memset(void *d,int c,unsigned long n){ unsigned char *p=d; for(unsigned long i=0;i<n;i++)p[i]=(unsigned char)c; }
static void fmt_uint(char *o,unsigned v){ char t[16]; int n=0; if(!v)t[n++]='0'; else while(v){t[n++]=(char)('0'+v%10);v/=10;} int i=0; while(n--)o[i++]=t[n]; o[i]='\0'; }
static void fmt_uint64(char *o,uint64_t v){ char t[24]; int n=0; if(!v)t[n++]='0'; else while(v){t[n++]=(char)('0'+(int)(v%10));v/=10;} int i=0; while(n--)o[i++]=t[n]; o[i]='\0'; }
static int str_append(char *b,int p,const char *s){ while(*s)b[p++]=*s++; return p; }
static int str_append_uint(char *b,int p,unsigned v){ char t[16]; fmt_uint(t,v); return str_append(b,p,t); }
static int str_append_uint64(char *b,int p,uint64_t v){ char t[24]; fmt_uint64(t,v); return str_append(b,p,t); }

/* ---- colours --------------------------------------------------------- */
#define COL_BG        0x001A1B26u
#define COL_TEXT      0x00C0CAF5u
#define COL_DIM       0x00565F89u
#define COL_ACCENT    0x007AA2F7u
#define COL_GREEN     0x009ECE6Au
#define COL_BLUE      0x007DCFFFu
#define COL_RED       0x00F7768Eu
#define COL_BAR_BG    0x00303340u
#define COL_GRAPH_BG  0x00252535u
#define COL_PANEL     0x00141520u

/* ---- state ----------------------------------------------------------- */
#define MAX_SVCS    64
#define HISTORY_LEN 16
enum { TAB_PROCESSES, TAB_PERFORMANCE, TAB_SERVICES };

static struct abi_service_info  g_svcs[MAX_SVCS];
static int  g_svc_count;
static struct abi_system_metrics g_metrics;
static struct abi_hwinfo_summary g_hw;   /* static CPU/machine identity */
static int  g_hw_ok;
static int  g_tab;
static uint32_t g_cpu_hist[HISTORY_LEN], g_ram_hist[HISTORY_LEN];
static int g_hist_idx, g_hist_filled;

static void refresh_data(void) {
    my_memset(&g_metrics, 0, sizeof(g_metrics));
    sys_system_metrics(&g_metrics);
    /* CPU/machine identity is static -- fetch once. */
    if (!g_hw_ok) {
        my_memset(&g_hw, 0, sizeof(g_hw));
        if (sys_hwinfo(&g_hw) == 0) g_hw_ok = 1;
    }
    my_memset(g_svcs, 0, sizeof(g_svcs));
    long n = sys_svc_list(g_svcs, MAX_SVCS);
    g_svc_count = (n > 0) ? (int)n : 0;
    g_cpu_hist[g_hist_idx] = g_metrics.cpu_pct;
    g_ram_hist[g_hist_idx] = g_metrics.ram_pct;
    g_hist_idx = (g_hist_idx + 1) % HISTORY_LEN;
    if (g_hist_filled < HISTORY_LEN) g_hist_filled++;
}

static const char *state_str(uint32_t s) {
    switch (s) { case 0:return "Stopped"; case 1:return "Running"; case 2:return "Crashed";
                 case 3:return "Failed"; case 4:return "Disabled"; default:return "Unknown"; }
}
static void format_uptime(char *buf, uint64_t ms) {
    unsigned total_s=(unsigned)(ms/1000), d=total_s/86400, h=(total_s/3600)%24, m=(total_s/60)%60, s=total_s%60;
    int p=0;
    if(d>0){ p=str_append_uint(buf,p,d); buf[p++]='d'; buf[p++]=' '; }
    p=str_append_uint(buf,p,h); buf[p++]='h'; buf[p++]=' ';
    p=str_append_uint(buf,p,m); buf[p++]='m'; buf[p++]=' ';
    p=str_append_uint(buf,p,s); buf[p++]='s'; buf[p]='\0';
}

/* ---- TobyTK UI ------------------------------------------------------- */
static struct tk_window win;
static struct tk_widget *content, *w_table, *w_status, *w_tabbtn[3], *w_canvas;
static int g_base;

static void rebuild(void);

/* table cell accessors (single rotating buffer; each cell consumed before next) */
static const char *proc_cell(void *u,int r,int c){
    static char b[32]; (void)u;
    if(r<0||r>=g_svc_count) return "";
    const struct abi_service_info *s=&g_svcs[r];
    switch(c){
    case 0: if(s->pid>=0)fmt_uint(b,(unsigned)s->pid); else {b[0]='-';b[1]='\0';} return b;
    case 1: return s->name;
    case 2: return state_str(s->state);
    case 3: {
        unsigned pp = s->state==1 ? (g_metrics.process_count>1 ? g_metrics.cpu_pct/g_metrics.process_count : g_metrics.cpu_pct) : 0;
        int p=str_append_uint(b,0,pp); b[p++]='%'; b[p]='\0'; return b;
    }}
    return "";
}
static const char *svc_cell(void *u,int r,int c){
    static char b[24]; (void)u;
    if(r<0||r>=g_svc_count) return "";
    const struct abi_service_info *s=&g_svcs[r];
    switch(c){
    case 0: return s->name;
    case 1: return state_str(s->state);
    case 2: if(s->pid>=0)fmt_uint(b,(unsigned)s->pid); else {b[0]='-';b[1]='\0';} return b;
    case 3: fmt_uint(b,s->restart_count); return b;
    case 4: fmt_uint(b,s->crash_count); return b;
    }
    return "";
}
static const char *const proc_hdr[]={"PID","Name","State","CPU%"};
static const int         proc_w[]  ={60,210,110,70};
static const char *const svc_hdr[] ={"Name","State","PID","Restarts","Crashes"};
static const int         svc_w[]   ={160,100,70,90,90};

/* ---- performance canvas ---------------------------------------------- */
static void draw_bar(struct tk_window *w,int x,int y,int mw,int h,uint32_t pct,uint32_t fg){
    tk_draw_fill(w,x,y,mw,h,COL_BAR_BG);
    int fw=(int)((pct*(uint32_t)mw)/100u); if(fw>mw)fw=mw;
    if(fw>0)tk_draw_fill(w,x,y,fw,h,fg);
}
static void draw_graph(struct tk_window *w,int x,int y,int gw,int gh,const uint32_t *hist,int count,int head,uint32_t bar){
    tk_draw_fill(w,x,y,gw,gh,COL_GRAPH_BG);
    if(count==0)return;
    int bw=gw/HISTORY_LEN; if(bw<2)bw=2; int eff=bw-1; if(eff<1)eff=1;
    for(int i=0;i<count&&i<HISTORY_LEN;i++){
        int idx=(head-count+i+HISTORY_LEN)%HISTORY_LEN;
        uint32_t pct=hist[idx]; if(pct>100)pct=100;
        int bh=(int)(pct*(uint32_t)gh/100u); if(bh<1&&pct>0)bh=1;
        if(bh>0)tk_draw_fill(w,x+i*bw,y+gh-bh,eff,bh,bar);
    }
}
static void perf_paint(struct tk_window *w,struct tk_widget *c){
    int ox,oy,cw,ch; tk_geom(c,&ox,&oy,&cw,&ch); (void)cw; (void)ch;
    tk_draw_fill(w,ox,oy,cw,ch,COL_BG);
    int lx=ox+20, y=oy+12, rx=ox+360;
    char buf[64];

    { int p=str_append(buf,0,"CPU Usage");
      if(g_hw_ok){ p=str_append(buf,p," ("); p=str_append_uint(buf,p,g_hw.cpu_count);
                   p=str_append(buf,p,g_hw.cpu_count==1?" core)":" cores)"); }
      buf[p]='\0'; }
    tk_draw_text(w,lx,y,buf,COL_ACCENT,15,1); y+=22;
    { int p=str_append_uint(buf,0,g_metrics.cpu_pct); buf[p++]='%'; buf[p]='\0'; }
    tk_draw_text(w,lx,y,buf,COL_GREEN,15,0);
    draw_bar(w,lx+60,y,250,12,g_metrics.cpu_pct,COL_GREEN); y+=24;
    tk_draw_text(w,lx,y,"History (16s)",COL_DIM,12,0); y+=16;
    draw_graph(w,lx,y,HISTORY_LEN*18,60,g_cpu_hist,g_hist_filled,g_hist_idx,COL_GREEN); y+=72;

    tk_draw_text(w,lx,y,"Memory Usage",COL_ACCENT,15,1); y+=22;
    { int p=str_append_uint(buf,0,g_metrics.ram_pct); buf[p++]='%'; buf[p]='\0'; }
    tk_draw_text(w,lx,y,buf,COL_BLUE,15,0);
    draw_bar(w,lx+60,y,250,12,g_metrics.ram_pct,COL_BLUE); y+=24;
    tk_draw_text(w,lx,y,"History (16s)",COL_DIM,12,0); y+=16;
    draw_graph(w,lx,y,HISTORY_LEN*18,60,g_ram_hist,g_hist_filled,g_hist_idx,COL_BLUE);

    int sy=oy+12;
    tk_draw_text(w,rx,sy,"System Info",COL_ACCENT,15,1); sy+=24;
    /* Real CPU identity for THIS machine (CPUID/SMP via SYS_HWINFO). */
    if(g_hw_ok){
        tk_draw_text(w,rx,sy,g_hw.cpu_brand[0]?g_hw.cpu_brand:"CPU: (unknown)",COL_TEXT,12,0); sy+=18;
        { int p=str_append(buf,0,"Cores:   "); p=str_append_uint(buf,p,g_hw.cpu_count);
          if(g_metrics.tsc_mhz){ p=str_append(buf,p,"  @ "); p=str_append_uint(buf,p,g_metrics.tsc_mhz); p=str_append(buf,p," MHz"); }
          buf[p]='\0'; }
        tk_draw_text(w,rx,sy,buf,COL_TEXT,13,0); sy+=24;
    }
    { int p=str_append(buf,0,"Uptime:  "); char u[32]; format_uptime(u,g_metrics.uptime_ms); p=str_append(buf,p,u); buf[p]='\0'; }
    tk_draw_text(w,rx,sy,buf,COL_TEXT,13,0); sy+=20;
    { int p=str_append(buf,0,"Pages:   "); p=str_append_uint64(buf,p,g_metrics.used_pages); p=str_append(buf,p," / "); p=str_append_uint64(buf,p,g_metrics.total_pages); buf[p]='\0'; }
    tk_draw_text(w,rx,sy,buf,COL_TEXT,13,0); sy+=20;
    { int p=str_append(buf,0,"Procs:   "); p=str_append_uint(buf,p,g_metrics.process_count); p=str_append(buf,p," ("); p=str_append_uint(buf,p,g_metrics.ready_count); p=str_append(buf,p," rdy)"); buf[p]='\0'; }
    tk_draw_text(w,rx,sy,buf,COL_TEXT,13,0); sy+=28;
    tk_draw_fill(w,rx,sy-8,280,1,COL_PANEL);
    tk_draw_text(w,rx,sy,"Counters /s",COL_ACCENT,15,1); sy+=24;
    { int p=str_append(buf,0,"Syscalls:  "); p=str_append_uint(buf,p,g_metrics.syscalls_per_s); buf[p]='\0'; }
    tk_draw_text(w,rx,sy,buf,COL_TEXT,13,0); sy+=20;
    { int p=str_append(buf,0,"Ctx sw:    "); p=str_append_uint64(buf,p,g_metrics.context_switches); buf[p]='\0'; }
    tk_draw_text(w,rx,sy,buf,COL_TEXT,13,0); sy+=20;
    { int p=str_append(buf,0,"GUI FPS:   "); p=str_append_uint(buf,p,g_metrics.gui_fps); buf[p]='\0'; }
    tk_draw_text(w,rx,sy,buf,COL_TEXT,13,0); sy+=20;
    { int p=str_append(buf,0,"VFS ops/s: "); p=str_append_uint(buf,p,g_metrics.vfs_ops_per_s); buf[p]='\0'; }
    tk_draw_text(w,rx,sy,buf,COL_TEXT,13,0); sy+=20;
    { int p=str_append(buf,0,"Net pkt/s: "); p=str_append_uint(buf,p,g_metrics.net_packets_per_s); buf[p]='\0'; }
    tk_draw_text(w,rx,sy,buf,COL_TEXT,13,0); sy+=20;
    { int p=str_append(buf,0,"TSC MHz:   "); p=str_append_uint(buf,p,g_metrics.tsc_mhz); buf[p]='\0'; }
    tk_draw_text(w,rx,sy,buf,COL_DIM,13,0);
}

/* ---- status / kill --------------------------------------------------- */
static void update_status(void){
    if(!w_status)return;
    char b[96]; int p=0;
    if(g_tab==TAB_SERVICES){
        int run=0,crash=0;
        for(int i=0;i<g_svc_count;i++){ if(g_svcs[i].state==1)run++; else if(g_svcs[i].state==2)crash++; }
        p=str_append(b,p,"Services: "); p=str_append_uint(b,p,(unsigned)g_svc_count);
        p=str_append(b,p,"   Running: "); p=str_append_uint(b,p,(unsigned)run);
        p=str_append(b,p,"   Crashed: "); p=str_append_uint(b,p,(unsigned)crash);
    } else {
        p=str_append(b,p,"Total: "); p=str_append_uint(b,p,(unsigned)g_svc_count);
        p=str_append(b,p,"   Procs: "); p=str_append_uint(b,p,g_metrics.process_count);
        p=str_append(b,p,"   Ready: "); p=str_append_uint(b,p,g_metrics.ready_count);
        p=str_append(b,p,"   Blocked: "); p=str_append_uint(b,p,g_metrics.blocked_count);
    }
    b[p]='\0';
    tk_set_text(&win,w_status,b);
}

static void on_kill(struct tk_window *w,struct tk_widget *b){
    (void)w;(void)b;
    if(g_tab!=TAB_PROCESSES||!w_table)return;
    int i=tk_table_selected(w_table);
    if(i>=0&&i<g_svc_count&&g_svcs[i].pid>=0&&g_svcs[i].state==1){
        sys_kill(g_svcs[i].pid);
        refresh_data();
        tk_table_rows(&win,w_table,g_svc_count,proc_cell,0);
        update_status();
        tk_redraw(&win);
    }
}

static void on_tab(struct tk_window *w,struct tk_widget *b){ (void)w; g_tab=(int)(long)b->user; rebuild(); }

static void rebuild(void){
    tk_clear_children(&win,win.root);
    tk_rewind(&win,g_base);
    win.capture=0; win.focus=0;
    content=w_table=w_status=w_canvas=0;

    struct tk_widget *root=tk_root(&win);
    tk_pad(root,0); root->gap=0;

    /* tab bar */
    struct tk_widget *tabs=tk_hbox(&win,root,2);
    tk_pad(tabs,4); tk_colors(tabs,COL_PANEL,0);
    static const char *names[3]={"Processes","Performance","Services"};
    for(int i=0;i<3;i++){
        w_tabbtn[i]=tk_button(&win,tabs,names[i],on_tab);
        w_tabbtn[i]->user=(void*)(long)i;
        if(i==g_tab)tk_colors(w_tabbtn[i],COL_ACCENT,0x00FFFFFF);
    }
    tk_grow(tk_label(&win,tabs,""),1);

    /* content */
    content=tk_vbox(&win,root,4); tk_pad(content,6); tk_grow(content,1);
    if(g_tab==TAB_PROCESSES){
        w_table=tk_table(&win,content,proc_hdr,proc_w,4);
        tk_table_rows(&win,w_table,g_svc_count,proc_cell,0);
        tk_grow(w_table,1);
        struct tk_widget *foot=tk_hbox(&win,content,8);
        w_status=tk_grow(tk_colors(tk_label(&win,foot,""),0,COL_DIM),1);
        tk_colors(tk_button(&win,foot,"Kill",on_kill),COL_RED,0x00FFFFFF);
    } else if(g_tab==TAB_SERVICES){
        w_table=tk_table(&win,content,svc_hdr,svc_w,5);
        tk_table_rows(&win,w_table,g_svc_count,svc_cell,0);
        tk_grow(w_table,1);
        w_status=tk_colors(tk_label(&win,content,""),0,COL_DIM);
    } else {
        w_canvas=tk_grow(tk_canvas(&win,content,perf_paint),1);
    }
    update_status();
    tk_redraw(&win);
}

static void on_key(struct tk_window *w,struct tk_event *ev){
    switch(ev->key){
    case 'q': case 27: tk_quit(w); break;
    case '1': g_tab=TAB_PROCESSES; rebuild(); break;
    case '2': g_tab=TAB_PERFORMANCE; rebuild(); break;
    case '3': g_tab=TAB_SERVICES; rebuild(); break;
    case '\t': g_tab=(g_tab+1)%3; rebuild(); break;
    case 'x': case 127: on_kill(w,0); break;
    default: break;
    }
}

int main(int argc,char **argv);
int main(int argc,char **argv){
    (void)argc;(void)argv;
    if(tk_window_open(&win,700,500,"Task Manager")!=0)return 1;
    tk_on_key(&win,on_key);
    g_tab=TAB_PROCESSES;
    refresh_data();
    g_base=tk_checkpoint(&win);
    rebuild();

    long last=sys_clock_ms();
    for(;;){
        if(tk_pump(&win))break;
        long now=sys_clock_ms();
        if(now-last>=1000){
            last=now;
            refresh_data();
            /* Per-widget dirty: the setters mark the table + status line, and
             * we mark the perf-tab canvas explicitly -- so the 1 s refresh
             * repaints only what changed, not the whole window. */
            if(w_table)tk_table_rows(&win,w_table,g_svc_count,g_tab==TAB_SERVICES?svc_cell:proc_cell,0);
            update_status();
            if(w_canvas)tk_dirty(&win,w_canvas);
        }
        usleep(40000);
    }
    return 0;
}
