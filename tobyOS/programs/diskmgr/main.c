/* diskmgr/main.c -- Disk Manager GUI for tobyOS.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The disk/partition model is
 * unchanged (block devices via ABI_SYS_DEV_LIST + a synthesized GPT layout for
 * the boot disk). The UI moved to TobyTK: a disk tk_listbox, a TK_CANVAS that
 * draws the proportional partition-layout bar, the partition table as a
 * TK_TABLE, Mount/Unmount/Properties buttons, and a Properties page (built by
 * rebuilding the window) with a usage bar.
 */

#include <stdio.h>
#include <string.h>
#include <tobyos/abi/abi.h>
#include <toby/tk.h>

static long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}

#define COL_ACCENT 0x0089B4FAu
#define COL_GREEN  0x00A6E3A1u
#define COL_YELLOW 0x00F9E2AFu
#define COL_RED    0x00F38BA8u
#define COL_DIM    0x006C7086u
#define COL_BG     0x001E1E2Eu
#define COL_HDR    0x00585B70u

#define MAX_DISKS 4
#define MAX_PARTS 8

struct partition {
    char     label[32], fs_type[16], mount_point[32];
    uint64_t size_mb, used_mb;
    int      mounted;
    unsigned colour;
};
struct disk {
    char     name[32], model[48], scheme[8];
    uint64_t total_mb;
    int      part_count;
    struct partition parts[MAX_PARTS];
};

static struct disk g_disks[MAX_DISKS];
static int g_disk_count, g_sel_disk, g_sel_part = -1, g_show_props;
static char g_disk_labels[MAX_DISKS][40];
/* listbox stores the items pointer -- must outlive the widget (NOT stack-local) */
static const char *g_disk_items[MAX_DISKS];

static unsigned part_colours[] = { 0x89B4FA,0xA6E3A1,0xF9E2AF,0xF38BA8,0xCBA6F7,0x74C7EC,0xFAB387,0x94E2D5 };

static void populate_disks(void){
    struct abi_dev_info devs[16];
    long n=sc3(ABI_SYS_DEV_LIST,(long)devs,16,0); if(n<0)n=0;
    g_disk_count=0; memset(g_disks,0,sizeof(g_disks));
    int di=0; struct disk *d=&g_disks[0];
    strncpy(d->name,"ide0",31); strncpy(d->model,"QEMU HARDDISK 16 MB",47);
    strncpy(d->scheme,"GPT",7); d->total_mb=16; d->part_count=3;
    strncpy(d->parts[0].label,"EFI System",31); strncpy(d->parts[0].fs_type,"FAT32",15);
    strncpy(d->parts[0].mount_point,"/boot",31); d->parts[0].size_mb=4; d->parts[0].used_mb=2; d->parts[0].mounted=1; d->parts[0].colour=part_colours[0];
    strncpy(d->parts[1].label,"tobyfs Data",31); strncpy(d->parts[1].fs_type,"tobyfs",15);
    strncpy(d->parts[1].mount_point,"/data",31); d->parts[1].size_mb=8; d->parts[1].used_mb=3; d->parts[1].mounted=1; d->parts[1].colour=part_colours[1];
    strncpy(d->parts[2].label,"FAT32 Volume",31); strncpy(d->parts[2].fs_type,"FAT32",15);
    strncpy(d->parts[2].mount_point,"/fat",31); d->parts[2].size_mb=4; d->parts[2].used_mb=1; d->parts[2].mounted=1; d->parts[2].colour=part_colours[2];
    di++;
    for(int i=0;i<(int)n&&di<MAX_DISKS;i++){
        if(devs[i].bus!=ABI_DEVT_BUS_BLK)continue;
        d=&g_disks[di]; snprintf(d->name,31,"blk%d",di); strncpy(d->model,devs[i].name,47);
        strncpy(d->scheme,"Raw",7); d->total_mb=devs[i].extra[0]/2048; if(!d->total_mb)d->total_mb=1;
        d->part_count=1; strncpy(d->parts[0].label,"Unpartitioned",31); strncpy(d->parts[0].fs_type,"raw",15);
        d->parts[0].size_mb=d->total_mb; d->parts[0].colour=part_colours[di%8]; di++;
    }
    if(di==0)di=1; g_disk_count=di;
    for(int i=0;i<g_disk_count;i++){
        char sz[16]; uint64_t mb=g_disks[i].total_mb;
        if(mb>=1024)snprintf(sz,sizeof(sz),"%u.%u GB",(unsigned)(mb/1024),(unsigned)((mb%1024)*10/1024));
        else snprintf(sz,sizeof(sz),"%u MB",(unsigned)mb);
        snprintf(g_disk_labels[i],sizeof(g_disk_labels[i]),"%s  %s  %s",g_disks[i].name,g_disks[i].scheme,sz);
    }
}

static void fmt_size(uint64_t mb,char *buf,int cap){
    if(mb>=1024)snprintf(buf,cap,"%u.%u GB",(unsigned)(mb/1024),(unsigned)((mb%1024)*10/1024));
    else snprintf(buf,cap,"%u MB",(unsigned)mb);
}

/* ── TobyTK UI ─────────────────────────────────────────────────────────── */
static struct tk_window win;
static struct tk_widget *w_list, *w_table, *w_status;
static int g_base;

static void rebuild(void);

static void bar_paint(struct tk_window *w,struct tk_widget *c){
    int x,y,bw,bh; tk_geom(c,&x,&y,&bw,&bh);
    tk_draw_fill(w,x,y,bw,bh,COL_HDR);
    if(g_sel_disk<0||g_sel_disk>=g_disk_count)return;
    struct disk *d=&g_disks[g_sel_disk];
    if(!d->total_mb)return;
    int xoff=0;
    for(int i=0;i<d->part_count;i++){
        int pw=(int)(d->parts[i].size_mb*(uint64_t)bw/d->total_mb);
        if(pw<2)pw=2; if(i==d->part_count-1)pw=bw-xoff; if(pw<0)pw=0;
        unsigned col=(i==g_sel_part)?0x00FFFFFFu:(0x00000000u|d->parts[i].colour);
        tk_draw_fill(w,x+xoff,y,pw,bh,col);
        if(pw>40)tk_draw_text(w,x+xoff+4,y+bh/2-7,d->parts[i].label,COL_BG,12,0);
        xoff+=pw;
    }
}

static const char *part_cell(void *u,int r,int c){
    (void)u; static char b[24];
    if(g_sel_disk<0||g_sel_disk>=g_disk_count)return "";
    struct disk *d=&g_disks[g_sel_disk];
    if(r<0||r>=d->part_count)return "";
    struct partition *p=&d->parts[r];
    switch(c){
    case 0: return p->label;
    case 1: return p->fs_type;
    case 2: fmt_size(p->size_mb,b,sizeof(b)); return b;
    case 3: if(!p->mounted)return "--"; fmt_size(p->used_mb,b,sizeof(b)); return b;
    case 4: return p->mounted?p->mount_point:"--";
    case 5: return p->mounted?"Mounted":"Offline";
    }
    return "";
}
static const char *const part_hdr[]={"Label","Type","Size","Used","Mount","Status"};
static const int         part_w[]  ={140,70,70,70,90,80};

static void on_refresh(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; populate_disks(); rebuild(); }
static void on_disk(struct tk_window *w,struct tk_widget *l){ (void)w; g_sel_disk=tk_selected(l); g_sel_part=-1; rebuild(); }
static void on_part(struct tk_window *w,struct tk_widget *t){ (void)w; g_sel_part=tk_table_selected(t); tk_redraw(&win); }
static void on_mount(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(g_sel_part>=0)g_disks[g_sel_disk].parts[g_sel_part].mounted=1; rebuild(); }
static void on_unmount(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(g_sel_part>=0)g_disks[g_sel_disk].parts[g_sel_part].mounted=0; rebuild(); }
static void on_props(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(g_sel_part>=0){ g_show_props=1; rebuild(); } }
static void on_back(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; g_show_props=0; rebuild(); }

static void prop_row(struct tk_widget *p,const char *k,const char *v,uint32_t vc){
    struct tk_widget *row=tk_hbox(&win,p,8);
    tk_size(tk_colors(tk_label(&win,row,k),0,COL_DIM),100,0);
    tk_colors(tk_label(&win,row,v),0,vc?vc:win.theme.text);
}

static void rebuild(void){
    tk_clear_children(&win,win.root);
    tk_rewind(&win,g_base);
    win.capture=0; win.focus=0;
    w_list=w_table=w_status=0;

    struct tk_widget *root=tk_root(&win);
    tk_pad(root,0); root->gap=0;

    struct tk_widget *tb=tk_hbox(&win,root,8);
    tk_pad(tb,6); tk_colors(tb,win.theme.panel_bg,0);
    tk_grow(tk_bold(tk_colors(tk_label(&win,tb,"Disk Manager"),0,COL_ACCENT)),1);
    if(g_show_props)tk_button(&win,tb,"Back",on_back);
    else tk_button(&win,tb,"Refresh",on_refresh);

    if(g_show_props&&g_sel_disk>=0&&g_sel_part>=0&&g_sel_part<g_disks[g_sel_disk].part_count){
        struct partition *p=&g_disks[g_sel_disk].parts[g_sel_part];
        struct tk_widget *pp=tk_vbox(&win,root,6); tk_pad(pp,18); tk_grow(pp,1);
        tk_bold(tk_font(tk_label(&win,pp,"Partition Properties"),16));
        tk_separator(&win,pp);
        char l[32];
        prop_row(pp,"Label:",p->label,0);
        prop_row(pp,"Filesystem:",p->fs_type,0);
        fmt_size(p->size_mb,l,sizeof(l)); prop_row(pp,"Size:",l,0);
        fmt_size(p->used_mb,l,sizeof(l)); prop_row(pp,"Used:",l,0);
        { uint64_t f=(p->size_mb>p->used_mb)?p->size_mb-p->used_mb:0; fmt_size(f,l,sizeof(l)); prop_row(pp,"Free:",l,COL_GREEN); }
        prop_row(pp,"Mount:",p->mounted?p->mount_point:"(not mounted)",p->mounted?COL_ACCENT:COL_DIM);
        prop_row(pp,"Status:",p->mounted?"Mounted":"Offline",p->mounted?COL_GREEN:COL_DIM);
        { int pct=p->size_mb?(int)(p->used_mb*100/p->size_mb):0; char u[16]; snprintf(u,sizeof(u),"%d%% used",pct);
          tk_colors(tk_label(&win,pp,u),0,pct>90?COL_RED:COL_DIM); }
        tk_grow(tk_label(&win,pp,""),1);
    } else {
        struct tk_widget *main=tk_hbox(&win,root,0); tk_grow(main,1);
        /* disk list */
        struct tk_widget *side=tk_vbox(&win,main,2); tk_pad(side,6); tk_size(side,200,0); tk_colors(side,0x00252536u,0);
        tk_colors(tk_label(&win,side,"Disks"),0,COL_DIM);
        for(int i=0;i<g_disk_count;i++)g_disk_items[i]=g_disk_labels[i];
        w_list=tk_listbox(&win,side,g_disk_items,g_disk_count);
        w_list->on_change=on_disk; w_list->sel=g_sel_disk; tk_grow(w_list,1);
        /* right: info + bar + table + actions */
        struct tk_widget *right=tk_vbox(&win,main,6); tk_pad(right,8); tk_grow(right,1);
        if(g_sel_disk>=0&&g_sel_disk<g_disk_count){
            struct disk *d=&g_disks[g_sel_disk];
            tk_label(&win,right,d->model);
            { char info[80],sz[16]; fmt_size(d->total_mb,sz,sizeof(sz));
              snprintf(info,sizeof(info),"%s  |  %s  |  %d partitions",d->name,sz,d->part_count);
              tk_colors(tk_label(&win,right,info),0,COL_DIM); }
            tk_size(tk_canvas(&win,right,bar_paint),0,40);
            w_table=tk_table(&win,right,part_hdr,part_w,6);
            tk_table_rows(&win,w_table,d->part_count,part_cell,0);
            w_table->on_change=on_part; if(g_sel_part>=0)w_table->sel=g_sel_part; tk_grow(w_table,1);
            struct tk_widget *acts=tk_hbox(&win,right,8);
            tk_colors(tk_button(&win,acts,"Mount",on_mount),COL_ACCENT,0x00181825u);
            tk_colors(tk_button(&win,acts,"Unmount",on_unmount),COL_YELLOW,0x00181825u);
            tk_button(&win,acts,"Properties",on_props);
        }
    }

    struct tk_widget *sb=tk_hbox(&win,root,0); tk_pad(sb,4); tk_colors(sb,win.theme.panel_bg,0);
    { char s[40]; snprintf(s,sizeof(s),"%d disk(s) detected",g_disk_count);
      w_status=tk_colors(tk_label(&win,sb,s),0,COL_DIM); }
    tk_redraw(&win);
}

static void on_key(struct tk_window *w,struct tk_event *ev){
    if(ev->key=='q'||ev->key==27){ if(g_show_props){g_show_props=0;rebuild();} else tk_quit(w); }
    else if(ev->key=='r'){ populate_disks(); rebuild(); }
    else if(ev->key=='p'){ on_props(w,0); }
}

int main(int argc,char **argv){
    (void)argc;(void)argv;
    populate_disks(); g_sel_disk=0;
    if(tk_window_open(&win,760,520,"Disk Manager")!=0)return 1;
    tk_on_key(&win,on_key);
    g_base=tk_checkpoint(&win);
    rebuild();
    return tk_run(&win);
}
