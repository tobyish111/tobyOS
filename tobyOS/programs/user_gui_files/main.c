/* user_gui_files/main.c -- /bin/gui_files, the GUI file manager.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). All filesystem logic is unchanged
 * from the original raw-gui_* version (directory listing + sort, copy/paste via
 * readfile/write, delete, mkdir, open = navigate / exec .elf / hand off to the
 * viewer). The UI is now toolkit-native: a button toolbar, a "Places" tk_listbox
 * sidebar, the directory shown in a TK_TABLE (Name + Size, with ".." as row 0),
 * and a status tk_label. Delete-confirm and New-folder are modal pages built by
 * rebuilding the window (tk_checkpoint / tk_rewind).
 */

#include <toby/tk.h>

typedef unsigned long usize;
typedef long          ssize_t;

#define SYS_WRITE           1
#define SYS_CLOSE           4
#define SYS_FS_READDIR     18
#define SYS_FS_READFILE    19
#define SYS_EXEC           20
#define SYS_OPEN           35
#define SYS_UNLINK         41
#define SYS_MKDIR          42

#define O_WRONLY    0x1
#define O_CREAT     0x040
#define O_TRUNC     0x200

#define SYS_FS_NAME_MAX    64
#define SYS_FS_TYPE_FILE   1
#define SYS_FS_TYPE_DIR    2

/* MUST match the kernel ABI struct (include/tobyos/syscall.h): the kernel's
 * sys_fs_readdir writes uid/gid/mode too, so omitting them makes each entry
 * 12 B short -- the kernel then strides by its (larger) size and OVERRUNS this
 * array once a directory has enough entries, clobbering the globals right
 * after g_entries[] (the widget pointers) and GP-faulting in tk_set_text. */
struct vfs_dirent_user {
    char     name[SYS_FS_NAME_MAX];
    uint32_t type, size, uid, gid, mode;
};

/* ---- syscall stubs (filesystem only) --------------------------------- */
static inline ssize_t sys_write_to(int fd,const void *b,usize n){ ssize_t r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_WRITE),"D"((long)fd),"S"(b),"d"(n):"rcx","r11","memory"); return r; }
static inline long sys_fs_readdir(const char *p,struct vfs_dirent_user *o,int cap,int off){ long r; register long r10 __asm__("r10")=off; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_FS_READDIR),"D"(p),"S"(o),"d"((long)cap),"r"(r10):"rcx","r11","memory"); return r; }
static inline long sys_fs_readfile(const char *p,void *o,usize cap){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_FS_READFILE),"D"(p),"S"(o),"d"(cap):"rcx","r11","memory"); return r; }
static inline long sys_exec(const char *p,const char *a){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_EXEC),"D"(p),"S"(a):"rcx","r11","memory"); return r; }
static inline long sys_open(const char *p,int fl,int m){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_OPEN),"D"(p),"S"((long)fl),"d"((long)m):"rcx","r11","memory"); return r; }
static inline long sys_close(int fd){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_CLOSE),"D"((long)fd):"rcx","r11","memory"); return r; }
static inline long sys_unlink(const char *p){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_UNLINK),"D"(p):"rcx","r11","memory"); return r; }
static inline long sys_mkdir(const char *p,int m){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_MKDIR),"D"(p),"S"((long)m):"rcx","r11","memory"); return r; }

/* ---- tiny libc ------------------------------------------------------- */
static usize my_strlen(const char *s){ const char *p=s; while(*p)p++; return (usize)(p-s); }
static void my_memset(void *d,int c,usize n){ unsigned char *p=d; for(usize i=0;i<n;i++)p[i]=(unsigned char)c; }
static void my_memcpy(void *d,const void *s,usize n){ unsigned char *a=d; const unsigned char *b=s; for(usize i=0;i<n;i++)a[i]=b[i]; }
static int streq(const char *a,const char *b){ while(*a&&*a==*b){a++;b++;} return *a=='\0'&&*b=='\0'; }
static int my_strcmp(const char *a,const char *b){ while(*a&&*a==*b){a++;b++;} return (int)(unsigned char)*a-(int)(unsigned char)*b; }
static void str_copy(char *d,const char *s,usize cap){ usize i=0; while(i<cap-1&&s[i]){d[i]=s[i];i++;} d[i]='\0'; }
static const char *find_ext(const char *name){ const char *dot=0; for(const char *c=name;*c;c++)if(*c=='.')dot=c; return dot; }

static void fmt_size(char *buf,usize cap,uint32_t bytes){
    const char *unit; uint32_t v;
    if(bytes<1024){ v=bytes; unit=" B"; }
    else if(bytes<1024*1024){ v=bytes/1024; unit=" KB"; }
    else { v=bytes/(1024*1024); unit=" MB"; }
    char tmp[12]; int n=0; if(v==0)tmp[n++]='0'; else while(v){tmp[n++]=(char)('0'+v%10);v/=10;}
    int i=0; for(int j=n-1;j>=0&&(usize)i+1<cap;j--)buf[i++]=tmp[j];
    for(int k=0;unit[k]&&(usize)i+1<cap;k++)buf[i++]=unit[k];
    buf[i]='\0';
}

/* ---- state (unchanged model) ----------------------------------------- */
#define ENTRIES_MAX 128
static struct vfs_dirent_user g_entries[ENTRIES_MAX];
static int  g_entry_count = 0;
static char g_path[256]  = "/";
static char g_status[80] = "";
static int  g_selected   = 0;
static char g_clipboard[256] = "";
static int  g_clip_is_file = 0;
static int  g_confirm_mode = 0;     /* 0=normal, 1=delete, 2=new folder */
static char g_input_buf[64] = "";
static int  g_input_len = 0;

static const char *g_sidebar_labels[] = { "Home","Root","Binaries","Config","Libraries","Repository","System" };
static const char *g_sidebar_paths[]  = { "/","/","/bin","/etc","/lib","/repo","/system" };
#define SIDEBAR_COUNT 7
static int g_sidebar_sel = 1;

/* ---- path helpers (unchanged) ---------------------------------------- */
static void path_join(char *dst,usize cap,const char *dir,const char *leaf){
    usize i=0; for(;dir[i]&&i+1<cap;i++)dst[i]=dir[i];
    if(i>0&&dst[i-1]!='/'){ if(i+1>=cap){dst[cap-1]='\0';return;} dst[i++]='/'; }
    for(usize j=0;leaf[j]&&i+1<cap;j++)dst[i++]=leaf[j]; dst[i]='\0';
}
static void path_pop(void){
    usize n=my_strlen(g_path); if(n<=1)return;
    if(g_path[n-1]=='/')g_path[--n]='\0';
    while(n>0&&g_path[n-1]!='/')g_path[--n]='\0';
    if(n==0){ g_path[0]='/'; g_path[1]='\0'; return; }
    if(n>1&&g_path[n-1]=='/')g_path[n-1]='\0';
}
static const char *path_basename(const char *path){
    const char *last=path; for(const char *p=path;*p;p++)if(*p=='/'&&*(p+1))last=p+1; return last;
}

static void sort_entries(void){
    for(int i=1;i<g_entry_count;i++){
        struct vfs_dirent_user tmp; my_memcpy(&tmp,&g_entries[i],sizeof(tmp));
        int j=i-1;
        while(j>=0){
            int a_dir=(g_entries[j].type==SYS_FS_TYPE_DIR), b_dir=(tmp.type==SYS_FS_TYPE_DIR);
            if(a_dir>b_dir)break;
            if(a_dir==b_dir&&my_strcmp(g_entries[j].name,tmp.name)<=0)break;
            my_memcpy(&g_entries[j+1],&g_entries[j],sizeof(tmp)); j--;
        }
        my_memcpy(&g_entries[j+1],&tmp,sizeof(tmp));
    }
}

static void set_status(const char *s){ str_copy(g_status,s,sizeof(g_status)); }

static void refresh_listing(void){
    my_memset(g_entries,0,sizeof(g_entries));
    g_entry_count=0; g_selected=0;
    long n=sys_fs_readdir(g_path,g_entries,ENTRIES_MAX,0);
    if(n<0){ g_entry_count=0; set_status("Error reading directory"); return; }
    if(n>ENTRIES_MAX)n=ENTRIES_MAX;   /* defensive: never index past g_entries[] */
    g_entry_count=(int)n; sort_entries();
    for(int i=0;i<SIDEBAR_COUNT;i++) if(streq(g_path,g_sidebar_paths[i])){ g_sidebar_sel=i; break; }
    int dirs=0,files=0;
    for(int i=0;i<g_entry_count;i++){ if(g_entries[i].type==SYS_FS_TYPE_DIR)dirs++; else files++; }
    char b[80]; int p=0;
    { char t[12]; int tn=0; uint32_t v=(uint32_t)dirs; if(!v)t[tn++]='0'; else while(v){t[tn++]=(char)('0'+v%10);v/=10;} for(int j=tn-1;j>=0;j--)b[p++]=t[j]; }
    { const char *s=" folders, "; for(int i=0;s[i];i++)b[p++]=s[i]; }
    { char t[12]; int tn=0; uint32_t v=(uint32_t)files; if(!v)t[tn++]='0'; else while(v){t[tn++]=(char)('0'+v%10);v/=10;} for(int j=tn-1;j>=0;j--)b[p++]=t[j]; }
    { const char *s=" files"; for(int i=0;s[i];i++)b[p++]=s[i]; }
    b[p]='\0'; set_status(b);
}

static const char *file_icon(const struct vfs_dirent_user *e){
    if(e->type==SYS_FS_TYPE_DIR)return "[D]";
    const char *ext=find_ext(e->name); if(!ext)return "[ ]";
    if(streq(ext,".elf"))return "[x]";
    if(streq(ext,".c")||streq(ext,".h"))return "[C]";
    if(streq(ext,".txt")||streq(ext,".md"))return "[T]";
    if(streq(ext,".conf")||streq(ext,".toml"))return "[=]";
    if(streq(ext,".tpkg"))return "[P]";
    if(streq(ext,".so")||streq(ext,".a")||streq(ext,".o"))return "[L]";
    if(streq(ext,".tar")||streq(ext,".img")||streq(ext,".iso"))return "[#]";
    if(streq(ext,".db"))return "[D]";
    return "[ ]";
}

/* ---- file operations (unchanged) ------------------------------------- */
static void do_copy(void){
    if(g_selected==0){ set_status("Cannot copy '..'"); return; }
    int idx=g_selected-1; if(idx>=g_entry_count)return;
    path_join(g_clipboard,sizeof(g_clipboard),g_path,g_entries[idx].name);
    g_clip_is_file=(g_entries[idx].type==SYS_FS_TYPE_FILE);
    set_status("Copied to clipboard");
}
static void do_paste(void){
    if(!g_clipboard[0]){ set_status("Clipboard empty"); return; }
    if(!g_clip_is_file){ set_status("Can only paste files"); return; }
    static char copy_buf[32768];
    long n=sys_fs_readfile(g_clipboard,copy_buf,sizeof(copy_buf));
    if(n<0){ set_status("Read failed"); return; }
    const char *bn=path_basename(g_clipboard);
    char dest[256]; path_join(dest,sizeof(dest),g_path,bn);
    long fdd=sys_open(dest,O_WRONLY|O_CREAT|O_TRUNC,0);
    if(fdd<0){ set_status("Create failed"); return; }
    sys_write_to((int)fdd,copy_buf,(usize)n); sys_close((int)fdd);
    set_status("File pasted");
}
static void do_delete(void){
    if(g_selected==0)return; int idx=g_selected-1; if(idx>=g_entry_count)return;
    char full[256]; path_join(full,sizeof(full),g_path,g_entries[idx].name);
    set_status(sys_unlink(full)==0?"Deleted":"Delete failed");
}
static void do_mkdir(void){
    if(g_input_len==0)return; g_input_buf[g_input_len]='\0';
    char full[256]; path_join(full,sizeof(full),g_path,g_input_buf);
    set_status(sys_mkdir(full,0)==0?"Folder created":"mkdir failed");
}
static int open_entry(int row){
    if(row==0){ path_pop(); return 1; }
    int idx=row-1; if(idx>=g_entry_count)return 0;
    const struct vfs_dirent_user *e=&g_entries[idx];
    if(e->type==SYS_FS_TYPE_DIR){ char next[256]; path_join(next,sizeof(next),g_path,e->name); str_copy(g_path,next,sizeof(g_path)); return 1; }
    char full[256]; path_join(full,sizeof(full),g_path,e->name);
    const char *ext=find_ext(e->name);
    if(ext&&streq(ext,".elf")){ set_status(sys_exec(full,0)==0?"Launched":"Exec failed"); return 0; }
    set_status(sys_exec("/bin/gui_viewer",full)==0?"Opened in viewer":"Open failed"); return 0;
}

/* ---- TobyTK UI ------------------------------------------------------- */
#define COL_ACCENT 0x004FC3F7u
#define COL_DANGER 0x00EF5350u

static struct tk_window win;
static struct tk_widget *content, *w_table, *w_path, *w_status, *w_list, *w_newdir;
static int g_base;

static void rebuild(void);

/* file table cell accessor (row 0 = "..") */
static const char *file_cell(void *u,int r,int c){
    static char b[96]; (void)u;
    if(r==0){ if(c==0)return "[^]  .."; return "DIR"; }
    int i=r-1; if(i<0||i>=g_entry_count)return "";
    const struct vfs_dirent_user *e=&g_entries[i];
    if(c==0){
        int p=0; const char *ic=file_icon(e); while(*ic)b[p++]=*ic++;
        b[p++]=' '; b[p++]=' ';
        for(const char *n=e->name;*n&&p<94;n++)b[p++]=*n; b[p]='\0'; return b;
    }
    if(e->type==SYS_FS_TYPE_DIR)return "DIR";
    fmt_size(b,sizeof(b),e->size); return b;
}
static const char *const file_hdr[]={"Name","Size"};
static const int         file_w[]  ={500,90};

static void update_view(void){
    if(w_path)   tk_set_text(&win,w_path,g_path);
    if(w_status) tk_set_text(&win,w_status,g_status);
    if(w_table){ w_table->sel=g_selected; tk_table_rows(&win,w_table,g_entry_count+1,file_cell,0); }
    if(w_list)   w_list->sel=g_sidebar_sel;
    tk_redraw(&win);
}

static void on_up    (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; path_pop(); refresh_listing(); update_view(); }
static void on_open  (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(open_entry(g_selected))refresh_listing(); update_view(); }
static void on_copy  (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; do_copy(); update_view(); }
static void on_paste (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; do_paste(); refresh_listing(); update_view(); }
static void on_refresh(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; refresh_listing(); update_view(); }
static void on_del   (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(g_selected>0){ g_confirm_mode=1; rebuild(); } }
static void on_newdir(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; g_confirm_mode=2; g_input_len=0; g_input_buf[0]='\0'; rebuild(); }

static void on_tbl(struct tk_window *w,struct tk_widget *t){ (void)w; g_selected=tk_table_selected(t); }
static void on_side(struct tk_window *w,struct tk_widget *l){
    (void)w; int i=tk_selected(l); if(i<0||i>=SIDEBAR_COUNT)return;
    g_sidebar_sel=i; str_copy(g_path,g_sidebar_paths[i],sizeof(g_path));
    refresh_listing(); update_view();
}

static void on_del_yes(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; do_delete(); g_confirm_mode=0; refresh_listing(); rebuild(); }
static void on_dlg_no (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; g_confirm_mode=0; set_status("Cancelled"); rebuild(); }
static void on_create (struct tk_window *w,struct tk_widget *b){
    (void)w;(void)b;
    const char *nn=w_newdir?tk_get_text(w_newdir):0;
    if(nn){ str_copy(g_input_buf,nn,sizeof(g_input_buf)); g_input_len=(int)my_strlen(g_input_buf); }
    do_mkdir(); g_confirm_mode=0; refresh_listing(); rebuild();
}

static void tbtn(struct tk_widget *bar,const char *label,tk_cb cb,uint32_t bg){
    struct tk_widget *b=tk_button(&win,bar,label,cb);
    if(bg)tk_colors(b,bg,0x00FFFFFF);
}

static void rebuild(void){
    tk_clear_children(&win,win.root);
    tk_rewind(&win,g_base);
    win.capture=0; win.focus=0;
    content=w_table=w_path=w_status=w_list=w_newdir=0;

    struct tk_widget *root=tk_root(&win);
    tk_pad(root,0); root->gap=0;

    /* toolbar */
    struct tk_widget *tb=tk_hbox(&win,root,4);
    tk_pad(tb,5); tk_colors(tb,win.theme.panel_bg,0);
    tbtn(tb,"Up",on_up,0);
    tbtn(tb,"Open",on_open,0);
    tbtn(tb,"Copy",on_copy,0);
    tbtn(tb,"Paste",on_paste,0);
    tbtn(tb,"Del",on_del,0x003D2020u);
    tbtn(tb,"New",on_newdir,0);
    tbtn(tb,"Refresh",on_refresh,0);

    /* path bar */
    struct tk_widget *pb=tk_hbox(&win,root,6);
    tk_pad(pb,4); tk_colors(pb,0x001A1A2Au,0);
    tk_colors(tk_label(&win,pb,"Path:"),0,win.theme.text_dim);
    w_path=tk_grow(tk_colors(tk_label(&win,pb,g_path),0,COL_ACCENT),1);

    if(g_confirm_mode==1){
        struct tk_widget *dlg=tk_vbox(&win,root,8); tk_pad(dlg,24); tk_grow(dlg,1);
        tk_bold(tk_colors(tk_label(&win,dlg,"Delete this file?"),0,COL_DANGER));
        if(g_selected>0&&g_selected-1<g_entry_count) tk_label(&win,dlg,g_entries[g_selected-1].name);
        struct tk_widget *row=tk_hbox(&win,dlg,8);
        tk_colors(tk_button(&win,row,"Yes, delete",on_del_yes),COL_DANGER,0x00FFFFFF);
        tk_button(&win,row,"Cancel",on_dlg_no);
        tk_grow(tk_label(&win,dlg,""),1);
    } else if(g_confirm_mode==2){
        struct tk_widget *dlg=tk_vbox(&win,root,8); tk_pad(dlg,24); tk_grow(dlg,1);
        tk_bold(tk_label(&win,dlg,"New folder name:"));
        w_newdir=tk_field(&win,dlg,"");
        w_newdir->on_click=on_create;
        struct tk_widget *row=tk_hbox(&win,dlg,8);
        tk_colors(tk_button(&win,row,"Create",on_create),COL_ACCENT,0x00181825u);
        tk_button(&win,row,"Cancel",on_dlg_no);
        tk_grow(tk_label(&win,dlg,""),1);
        win.focus=w_newdir; w_newdir->focused=1;
    } else {
        struct tk_widget *main=tk_hbox(&win,root,0); tk_grow(main,1);
        /* places sidebar */
        struct tk_widget *side=tk_vbox(&win,main,2); tk_pad(side,6); tk_size(side,150,0);
        tk_colors(side,0x00252536u,0);
        tk_colors(tk_label(&win,side,"Places"),0,COL_ACCENT);
        w_list=tk_listbox(&win,side,g_sidebar_labels,SIDEBAR_COUNT);
        w_list->on_change=on_side; w_list->sel=g_sidebar_sel; tk_grow(w_list,1);
        /* file table */
        struct tk_widget *right=tk_vbox(&win,main,0); tk_pad(right,4); tk_grow(right,1);
        w_table=tk_table(&win,right,file_hdr,file_w,2);
        tk_table_rows(&win,w_table,g_entry_count+1,file_cell,0);
        w_table->on_change=on_tbl; w_table->sel=g_selected; tk_grow(w_table,1);
    }

    /* status bar */
    struct tk_widget *sb=tk_hbox(&win,root,0); tk_pad(sb,4); tk_colors(sb,win.theme.panel_bg,0);
    w_status=tk_colors(tk_label(&win,sb,g_status),0,win.theme.text_dim);

    tk_redraw(&win);
}

int main(int argc,char **argv);
int main(int argc,char **argv){
    (void)argc;(void)argv;
    if(tk_window_open(&win,760,480,"File Explorer")!=0)return 1;
    g_base=tk_checkpoint(&win);
    refresh_listing();
    rebuild();
    return tk_run(&win);
}
