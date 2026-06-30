/* programs/devmgr/main.c -- Device Manager for tobyOS.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The device model is unchanged: it
 * queries the kernel PnP list via ABI_SYS_DEV_LIST (falling back to a platform
 * device set) and classifies each device. The UI moved to TobyTK: a toolbar
 * (Refresh / Properties), the devices in a TK_TABLE (Class / Device / Driver /
 * Status), and a summary tk_label. Selecting a row + Properties opens a details
 * page (built by rebuilding the window) with an Enable/Disable toggle.
 */

#include <stdio.h>
#include <string.h>
#include <tobyos/abi/abi.h>
#include <toby/tk.h>

static long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}

/* ── colours ─────────────────────────────────────────────────────────── */
#define COL_ACCENT 0x0089B4FAu
#define COL_GREEN  0x00A6E3A1u
#define COL_YELLOW 0x00F9E2AFu
#define COL_RED    0x00F38BA8u
#define COL_DIM    0x006C7086u

/* ── device data (unchanged model) ───────────────────────────────────── */
enum dev_class { CLS_DISPLAY, CLS_NETWORK, CLS_STORAGE, CLS_AUDIO, CLS_INPUT, CLS_USB, CLS_OTHER, CLS_COUNT };
static const char *cls_names[] = {
    "Display", "Network", "Storage", "Audio", "Input", "USB", "Other"
};

struct device {
    char name[48];
    char driver[24];
    int  status;     /* 0=OK, 1=warning, 2=error, 3=disabled */
    int  cls;
    int  vid, pid;
    const char *bus;
    int  irq;
};
static struct device g_devs[32];
static int g_dev_count;
static int g_selected = -1;
static int g_show_props = 0;

static int classify_pci(uint8_t cc){
    switch(cc){
    case 0x03:return CLS_DISPLAY; case 0x02:return CLS_NETWORK; case 0x01:return CLS_STORAGE;
    case 0x04:return CLS_AUDIO; case 0x09:return CLS_INPUT; case 0x0C:return CLS_USB;
    default:return CLS_OTHER; }
}

static void populate_devices(void){
    memset(g_devs,0,sizeof(g_devs)); g_dev_count=0;
    struct abi_dev_info devs[32];
    long n=sc3(ABI_SYS_DEV_LIST,(long)devs,32,0);
    if(n>0){
        for(int i=0;i<(int)n&&g_dev_count<32;i++){
            struct device *d=&g_devs[g_dev_count];
            strncpy(d->name,devs[i].name,47); strncpy(d->driver,devs[i].driver,23);
            d->vid=devs[i].vendor; d->pid=devs[i].device; d->irq=0;
            d->status=(devs[i].driver[0])?0:2;
            if(devs[i].bus==ABI_DEVT_BUS_PCI){ d->bus="PCI"; d->cls=classify_pci(devs[i].class_code); }
            else if(devs[i].bus==ABI_DEVT_BUS_USB){ d->bus="USB"; d->cls=CLS_USB; if(devs[i].class_code==0x08)d->cls=CLS_STORAGE; }
            else if(devs[i].bus==ABI_DEVT_BUS_BLK){ d->bus="Block"; d->cls=CLS_STORAGE; }
            else { d->bus="Platform"; d->cls=CLS_OTHER; }
            g_dev_count++;
        }
    }
    if(g_dev_count==0){
        struct { const char *n,*d; int s,c,v,p; const char *b; int irq; } fb[]={
            {"VGA Compatible Controller","vga",0,CLS_DISPLAY,0x1234,0x1111,"PCI",11},
            {"Intel Ethernet I219-LM","e1000",0,CLS_NETWORK,0x8086,0x100E,"PCI",10},
            {"AHCI SATA Controller","ahci",0,CLS_STORAGE,0x8086,0x2922,"PCI",14},
            {"Intel HD Audio","hda",0,CLS_AUDIO,0x8086,0x2668,"PCI",5},
            {"PS/2 Keyboard","i8042",0,CLS_INPUT,0,0,"Platform",1},
            {"PS/2 Mouse","i8042",0,CLS_INPUT,0,0,"Platform",12},
            {"USB xHCI Controller","xhci",0,CLS_USB,0x8086,0xA12F,"PCI",9},
        };
        g_dev_count=(int)(sizeof(fb)/sizeof(fb[0]));
        for(int i=0;i<g_dev_count;i++){
            strncpy(g_devs[i].name,fb[i].n,47); strncpy(g_devs[i].driver,fb[i].d,23);
            g_devs[i].status=fb[i].s; g_devs[i].cls=fb[i].c; g_devs[i].vid=fb[i].v;
            g_devs[i].pid=fb[i].p; g_devs[i].bus=fb[i].b; g_devs[i].irq=fb[i].irq;
        }
    }
}

static const char *status_str(int s){ switch(s){case 0:return "OK";case 1:return "Warning";case 2:return "Error";case 3:return "Disabled";} return "?"; }

/* ── TobyTK UI ─────────────────────────────────────────────────────────── */
static struct tk_window win;
static struct tk_widget *content, *w_table, *w_summary;
static int g_base;

static void rebuild(void);

static const char *dev_cell(void *u,int r,int c){
    (void)u;
    if(r<0||r>=g_dev_count)return "";
    const struct device *d=&g_devs[r];
    switch(c){
    case 0: return cls_names[d->cls];
    case 1: return d->name;
    case 2: return d->driver;
    case 3: return status_str(d->status);
    }
    return "";
}
static const char *const dev_hdr[]={"Class","Device","Driver","Status"};
static const int         dev_w[]  ={90,300,120,90};

static void on_refresh(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; populate_devices(); if(w_table){ tk_table_rows(&win,w_table,g_dev_count,dev_cell,0);} rebuild(); }
static void on_props(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(w_table)g_selected=tk_table_selected(w_table); if(g_selected>=0&&g_selected<g_dev_count){ g_show_props=1; rebuild(); } }
static void on_back(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; g_show_props=0; rebuild(); }
static void on_toggle(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(g_selected>=0&&g_selected<g_dev_count){ g_devs[g_selected].status=(g_devs[g_selected].status==3)?0:3; rebuild(); } }
static void on_tbl(struct tk_window *w,struct tk_widget *t){ (void)w; g_selected=tk_table_selected(t); }

static void prop_row(struct tk_widget *p,const char *k,const char *v,uint32_t vc){
    struct tk_widget *row=tk_hbox(&win,p,8);
    tk_size(tk_colors(tk_label(&win,row,k),0,COL_DIM),110,0);
    tk_colors(tk_label(&win,row,v),0,vc?vc:win.theme.text);
}

static void rebuild(void){
    tk_clear_children(&win,win.root);
    tk_rewind(&win,g_base);
    win.capture=0; win.focus=0;
    content=w_table=w_summary=0;

    struct tk_widget *root=tk_root(&win);
    tk_pad(root,0); root->gap=0;

    /* toolbar */
    struct tk_widget *tb=tk_hbox(&win,root,8);
    tk_pad(tb,6); tk_colors(tb,win.theme.panel_bg,0);
    tk_grow(tk_bold(tk_colors(tk_label(&win,tb,"Device Manager"),0,COL_ACCENT)),1);
    if(g_show_props){ tk_button(&win,tb,"Back",on_back); }
    else { tk_button(&win,tb,"Properties",on_props); tk_button(&win,tb,"Refresh",on_refresh); }

    if(g_show_props&&g_selected>=0&&g_selected<g_dev_count){
        struct device *d=&g_devs[g_selected];
        struct tk_widget *p=tk_vbox(&win,root,6); tk_pad(p,18); tk_grow(p,1);
        tk_bold(tk_font(tk_label(&win,p,"Device Properties"),16));
        tk_separator(&win,p);
        prop_row(p,"Name:",d->name,0);
        prop_row(p,"Driver:",d->driver,0);
        { uint32_t sc=d->status==0?COL_GREEN:d->status==1?COL_YELLOW:d->status==2?COL_RED:COL_DIM;
          prop_row(p,"Status:",status_str(d->status),sc); }
        prop_row(p,"Bus:",d->bus,0);
        { char vp[24]; snprintf(vp,sizeof(vp),"%04X:%04X",d->vid,d->pid); prop_row(p,"VID:PID:",vp,0); }
        { char ir[16]; snprintf(ir,sizeof(ir),"%d",d->irq); prop_row(p,"IRQ:",ir,0); }
        tk_grow(tk_label(&win,p,""),1);
        struct tk_widget *row=tk_hbox(&win,p,8);
        const char *lbl=(d->status==3)?"Enable":"Disable";
        tk_colors(tk_button(&win,row,lbl,on_toggle),d->status==3?COL_GREEN:COL_RED,0x00181825u);
    } else {
        content=tk_vbox(&win,root,4); tk_pad(content,6); tk_grow(content,1);
        w_table=tk_table(&win,content,dev_hdr,dev_w,4);
        tk_table_rows(&win,w_table,g_dev_count,dev_cell,0);
        w_table->on_change=on_tbl; if(g_selected>=0)w_table->sel=g_selected; tk_grow(w_table,1);
        struct tk_widget *sb=tk_hbox(&win,root,0); tk_pad(sb,4); tk_colors(sb,win.theme.panel_bg,0);
        char s[40]; snprintf(s,sizeof(s),"%d devices total",g_dev_count);
        w_summary=tk_colors(tk_label(&win,sb,s),0,COL_DIM);
    }
    tk_redraw(&win);
}

static void on_key(struct tk_window *w,struct tk_event *ev){
    if(ev->key=='q'||ev->key==27){ if(g_show_props){g_show_props=0;rebuild();} else tk_quit(w); }
    else if(ev->key=='r'){ populate_devices(); rebuild(); }
    else if(ev->key=='p'){ on_props(w,0); }
}

int main(int argc,char **argv){
    (void)argc;(void)argv;
    populate_devices();
    if(tk_window_open(&win,720,500,"Device Manager")!=0)return 1;
    tk_on_key(&win,on_key);
    g_base=tk_checkpoint(&win);
    rebuild();
    return tk_run(&win);
}
