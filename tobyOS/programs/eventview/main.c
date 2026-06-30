/* eventview/main.c -- System Event Viewer for tobyOS.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The log model is unchanged: it
 * reads the kernel slog ring via ABI_SYS_SLOG_READ and filters by severity /
 * subsystem / search substring. The UI moved to TobyTK: severity + subsystem
 * filter buttons (active one accented), a search tk_field, the entries in a
 * TK_TABLE (Time / Level / PID / Subsystem / Message), and a status tk_label.
 * The app drives its own loop (tk_pump) to fetch new entries ~1s and to apply
 * the live search field.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <tobyos/abi/abi.h>
#include <toby/tk.h>

static long sc0(long n){long r;__asm__ volatile("syscall":"=a"(r):"0"(n):"rcx","r11","memory");return r;}
static long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static long sys_clock_ms(void){ return sc0(ABI_SYS_CLOCK_MS); }

#define COL_ACCENT 0x0089B4FAu
#define COL_GREEN  0x00A6E3A1u
#define COL_YELLOW 0x00F9E2AFu
#define COL_RED    0x00F38BA8u
#define COL_DIM    0x006C7086u

#define MAX_ENTRIES 256
struct log_entry {
    uint64_t time_ms; uint32_t level; int32_t pid;
    char sub[ABI_SLOG_SUB_MAX]; char msg[ABI_SLOG_MSG_MAX];
};
static struct log_entry g_entries[MAX_ENTRIES];
static int g_entry_count;
static uint64_t g_last_seq;

static int g_filtered[MAX_ENTRIES];
static int g_filtered_count;

static int g_level_filter = -1;     /* -1 all, 0..3 level */
#define NUM_SUBSYSTEMS 7
static const char *g_subsystems[] = {"All","net","fs","driver","sched","boot","audio"};
static int g_sub_filter;
static char g_search[32];

static void fetch_logs(void){
    struct abi_slog_record recs[64];
    long n=sc3(ABI_SYS_SLOG_READ,(long)recs,64,(long)g_last_seq);
    if(n<=0)return;
    for(long i=0;i<n&&g_entry_count<MAX_ENTRIES;i++){
        struct log_entry *e=&g_entries[g_entry_count];
        e->time_ms=recs[i].time_ms; e->level=recs[i].level; e->pid=recs[i].pid;
        memcpy(e->sub,recs[i].sub,ABI_SLOG_SUB_MAX); memcpy(e->msg,recs[i].msg,ABI_SLOG_MSG_MAX);
        g_last_seq=recs[i].seq; g_entry_count++;
    }
}

static int entry_matches(struct log_entry *e){
    if(g_level_filter>=0&&(int)e->level!=g_level_filter)return 0;
    if(g_sub_filter>0){
        const char *want=g_subsystems[g_sub_filter]; int match=0;
        for(int i=0;want[i]&&e->sub[i];i++){ if(want[i]!=e->sub[i])break; if(!want[i+1]){match=1;break;} }
        if(!match)return 0;
    }
    if(g_search[0]){ if(!strstr(e->msg,g_search)&&!strstr(e->sub,g_search))return 0; }
    return 1;
}
static void rebuild_filter(void){
    g_filtered_count=0;
    for(int i=0;i<g_entry_count;i++) if(entry_matches(&g_entries[i])) g_filtered[g_filtered_count++]=i;
}

static const char *level_name(uint32_t lv){ switch(lv){case 0:return "ERROR";case 1:return "WARN";case 2:return "INFO";case 3:return "DEBUG";} return "?"; }

/* ── TobyTK UI ─────────────────────────────────────────────────────────── */
static struct tk_window win;
static struct tk_widget *w_table, *w_search, *w_status, *w_sev[5], *w_sub[NUM_SUBSYSTEMS];
static int g_base;

static const char *log_cell(void *u,int r,int c){
    (void)u; static char b[24];
    if(r<0||r>=g_filtered_count)return "";
    struct log_entry *e=&g_entries[g_filtered[r]];
    switch(c){
    case 0:{ unsigned s=(unsigned)(e->time_ms/1000),ms=(unsigned)(e->time_ms%1000);
             snprintf(b,sizeof(b),"%02u:%02u.%03u",s/60,s%60,ms); return b; }
    case 1: return level_name(e->level);
    case 2: if(e->pid<0)return "kern"; snprintf(b,sizeof(b),"%d",e->pid); return b;
    case 3: return e->sub;
    case 4: return e->msg;
    }
    return "";
}
static const char *const log_hdr[]={"Time","Level","PID","Subsys","Message"};
static const int         log_w[]  ={90,60,50,90,400};

static void rebuild(void);

static void on_sev(struct tk_window *w,struct tk_widget *b){ (void)w; g_level_filter=(int)(long)b->user; rebuild_filter(); rebuild(); }
static void on_sub(struct tk_window *w,struct tk_widget *b){ (void)w; g_sub_filter=(int)(long)b->user; rebuild_filter(); rebuild(); }

static void rebuild(void){
    tk_clear_children(&win,win.root);
    tk_rewind(&win,g_base);
    win.capture=0; win.focus=0;
    w_table=w_search=w_status=0;

    struct tk_widget *root=tk_root(&win);
    tk_pad(root,0); root->gap=0;

    /* toolbar: title + severity filters */
    struct tk_widget *tb=tk_hbox(&win,root,4);
    tk_pad(tb,5); tk_colors(tb,win.theme.panel_bg,0);
    tk_bold(tk_colors(tk_label(&win,tb,"Event Viewer"),0,COL_ACCENT));
    tk_label(&win,tb,"  ");
    static const char *sev[5]={"All","Error","Warn","Info","Debug"};
    static const int   sevv[5]={-1,0,1,2,3};
    for(int i=0;i<5;i++){
        w_sev[i]=tk_button(&win,tb,sev[i],on_sev);
        w_sev[i]->user=(void*)(long)sevv[i];
        if(g_level_filter==sevv[i])tk_colors(w_sev[i],COL_ACCENT,0x00181825u);
    }
    tk_grow(tk_label(&win,tb,""),1);

    /* subsystem filters + search */
    struct tk_widget *sbar=tk_hbox(&win,root,4);
    tk_pad(sbar,5);
    for(int i=0;i<NUM_SUBSYSTEMS;i++){
        w_sub[i]=tk_button(&win,sbar,g_subsystems[i],on_sub);
        w_sub[i]->user=(void*)(long)i;
        if(g_sub_filter==i)tk_colors(w_sub[i],COL_ACCENT,0x00181825u);
    }
    tk_grow(tk_label(&win,sbar,""),1);
    w_search=tk_field(&win,sbar,g_search);
    tk_size(w_search,200,0);

    /* table */
    struct tk_widget *body=tk_vbox(&win,root,0); tk_pad(body,4); tk_grow(body,1);
    w_table=tk_table(&win,body,log_hdr,log_w,5);
    tk_table_rows(&win,w_table,g_filtered_count,log_cell,0);
    tk_grow(w_table,1);

    /* status */
    struct tk_widget *st=tk_hbox(&win,root,0); tk_pad(st,4); tk_colors(st,win.theme.panel_bg,0);
    char s[64]; snprintf(s,sizeof(s),"%d entries  (%d shown)",g_entry_count,g_filtered_count);
    w_status=tk_colors(tk_label(&win,st,s),0,COL_DIM);

    win.focus=w_search; w_search->focused=1;
    tk_redraw(&win);
}

int main(int argc,char **argv){
    (void)argc;(void)argv;
    memset(g_entries,0,sizeof(g_entries));
    g_entry_count=0; g_last_seq=0; g_search[0]='\0';
    fetch_logs(); rebuild_filter();

    if(tk_window_open(&win,800,550,"Event Viewer")!=0)return 1;
    g_base=tk_checkpoint(&win);
    rebuild();

    long last=sys_clock_ms();
    char prev_search[32]="";
    for(;;){
        if(tk_pump(&win))break;
        /* live search: pick up the field text */
        if(w_search){
            const char *s=tk_get_text(w_search);
            if(strcmp(s,prev_search)!=0){
                strncpy(prev_search,s,sizeof(prev_search)-1); prev_search[sizeof(prev_search)-1]='\0';
                strncpy(g_search,s,sizeof(g_search)-1); g_search[sizeof(g_search)-1]='\0';
                rebuild_filter();
                if(w_table)tk_table_rows(&win,w_table,g_filtered_count,log_cell,0);
                { char st[64]; snprintf(st,sizeof(st),"%d entries  (%d shown)",g_entry_count,g_filtered_count);
                  if(w_status)tk_set_text(&win,w_status,st); }
                tk_redraw(&win);
            }
        }
        long now=sys_clock_ms();
        if(now-last>1000){
            last=now; int old=g_entry_count; fetch_logs();
            if(g_entry_count!=old){
                rebuild_filter();
                if(w_table)tk_table_rows(&win,w_table,g_filtered_count,log_cell,0);
                { char st[64]; snprintf(st,sizeof(st),"%d entries  (%d shown)",g_entry_count,g_filtered_count);
                  if(w_status)tk_set_text(&win,w_status,st); }
                tk_redraw(&win);
            }
        }
        usleep(40000);
    }
    return 0;
}
