/* user_gui_files/main.c -- /bin/gui_files, the GUI file manager.
 *
 * Explorer-style file manager on TobyTK: toolbar, Places sidebar, the
 * directory as a Name/Type/Size TK_TABLE, and a status bar.
 *
 * File operations:
 *   - Open: double-click or the Open button/context item. Directories
 *     navigate; .elf executes; text-ish files open in the editor
 *     (/bin/gui_edit); everything else goes to the viewer.
 *   - Edit: always opens the file in /bin/gui_edit.
 *   - New File / New Folder: modal name prompt, then creat()/mkdir().
 *   - Copy / Paste: chunked fd-to-fd copy (any size -- the old
 *     readfile-into-a-32KB-buffer path silently TRUNCATED bigger
 *     files), with Windows-style " (copy)" de-collision when pasting
 *     into the source directory or over an existing name.
 *   - Copy file path: puts the absolute path on the SYSTEM clipboard
 *     (ABI_SYS_CLIP_COPY), pasteable anywhere.
 *   - Delete: confirm page, then unlink.
 *
 * Right-click (or the keyboard Menu key) on a row opens a TobyTK
 * context menu -- tk_on_context pre-selects the row under the cursor,
 * so the menu always targets what was clicked.
 */

#include <toby/tk.h>
/* The REAL ABI header, not a hand-mirrored copy: struct abi_blk_info's
 * layout is frozen by a static assert that only applies to the original,
 * and the Format action below depends on it byte for byte. */
#include <tobyos/abi/abi.h>

typedef unsigned long usize;
typedef long          ssize_t;

#define SYS_WRITE           1
#define SYS_READ            2
#define SYS_CLOSE           4
#define SYS_FS_READDIR     18
#define SYS_EXEC           20
#define SYS_OPEN           35
#define SYS_UNLINK         41
#define SYS_MKDIR          42
#define SYS_CLOCK_MS       48
#define SYS_CLIP_COPY      77

#define O_RDONLY    0x0
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

/* ---- syscall stubs ---------------------------------------------------- */
static inline ssize_t sys_write_to(int fd,const void *b,usize n){ ssize_t r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_WRITE),"D"((long)fd),"S"(b),"d"(n):"rcx","r11","memory"); return r; }
static inline ssize_t sys_read_fd(int fd,void *b,usize n){ ssize_t r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_READ),"D"((long)fd),"S"(b),"d"(n):"rcx","r11","memory"); return r; }
static inline long sys_fs_readdir(const char *p,struct vfs_dirent_user *o,int cap,int off){ long r; register long r10 __asm__("r10")=off; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_FS_READDIR),"D"(p),"S"(o),"d"((long)cap),"r"(r10):"rcx","r11","memory"); return r; }
static inline long sys_exec(const char *p,const char *a){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_EXEC),"D"(p),"S"(a):"rcx","r11","memory"); return r; }
static inline long sys_open(const char *p,int fl,int m){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_OPEN),"D"(p),"S"((long)fl),"d"((long)m):"rcx","r11","memory"); return r; }
static inline long sys_close(int fd){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_CLOSE),"D"((long)fd):"rcx","r11","memory"); return r; }
static inline long sys_unlink(const char *p){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_UNLINK),"D"(p):"rcx","r11","memory"); return r; }
static inline long sys_mkdir(const char *p,int m){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_MKDIR),"D"(p),"S"((long)m):"rcx","r11","memory"); return r; }
static inline long sys_clock_ms(void){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_CLOCK_MS):"rcx","r11","memory"); return r; }
static inline long sys_clip_copy(const char *s,usize n){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_CLIP_COPY),"D"(s),"S"(n):"rcx","r11","memory"); return r; }
/* Format USB: the block-device list and the provisioning request. */
static inline long sys_blk_list(void *out,long cap){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)ABI_SYS_BLK_LIST),"D"(out),"S"(cap):"rcx","r11","memory"); return r; }
static inline long sys_provision(const void *req){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)ABI_SYS_DATA_PROVISION),"D"(req):"rcx","r11","memory"); return r; }

/* ---- tiny libc --------------------------------------------------------- */
static usize my_strlen(const char *s){ const char *p=s; while(*p)p++; return (usize)(p-s); }
static void my_memset(void *d,int c,usize n){ unsigned char *p=d; for(usize i=0;i<n;i++)p[i]=(unsigned char)c; }
static void my_memcpy(void *d,const void *s,usize n){ unsigned char *a=d; const unsigned char *b=s; for(usize i=0;i<n;i++)a[i]=b[i]; }
static int streq(const char *a,const char *b){ while(*a&&*a==*b){a++;b++;} return *a=='\0'&&*b=='\0'; }
static int my_strcmp(const char *a,const char *b){ while(*a&&*a==*b){a++;b++;} return (int)(unsigned char)*a-(int)(unsigned char)*b; }
static void str_copy(char *d,const char *s,usize cap){ usize i=0; while(i<cap-1&&s[i]){d[i]=s[i];i++;} d[i]='\0'; }
static const char *find_ext(const char *name){ const char *dot=0; for(const char *c=name;*c;c++)if(*c=='.')dot=c; return dot; }
static int str_cat(char *d,int p,usize cap,const char *s){ for(usize j=0;s[j]&&(usize)p+1<cap;j++)d[p++]=s[j]; d[p]='\0'; return p; }

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

/* ---- state -------------------------------------------------------------- */
#define ENTRIES_MAX 128
static struct vfs_dirent_user g_entries[ENTRIES_MAX];
static int  g_entry_count = 0;
/* Start in /data -- the user's writable, persistent volume (the "/"
 * ramfs root is read-only initrd content). The Root place is one
 * click away in the sidebar. */
static char g_path[256]  = "/data";
static char g_status[96] = "";
static int  g_selected   = 0;
static char g_clipboard[256] = "";
static int  g_clip_is_file = 0;
static int  g_confirm_mode = 0;     /* 0=normal, 1=delete, 2=new folder, 3=new file,
                                     * 4=pick a USB to format, 5=confirm format */

/* ---- Format USB ---------------------------------------------------------
 *
 * A file manager is where people look for their USB stick, so it is where
 * "format it" belongs -- the disk manager exists but nobody finds it, which
 * is the same reason /data sat RAM-backed for months while the provisioning
 * machinery was right there.
 *
 * This program decides NOTHING about safety. It shows what the kernel
 * already reports (removable, mounted, what filesystem is on it) and sends
 * a request; the kernel re-runs its guard and refuses fixed disks, the live
 * boot medium and mounted volumes whatever this asks for. What the GUI owns
 * is FRICTION: a destructive button one click from a file listing needs the
 * same typed confirmation the CLI asks for, because a mis-click is far
 * likelier here than a mis-typed command. */
#define FMT_DEVS_MAX 16
static struct abi_blk_info g_fmt_devs[FMT_DEVS_MAX];
static const char *g_fmt_labels[FMT_DEVS_MAX];
static char  g_fmt_label_buf[FMT_DEVS_MAX][64];
static int   g_fmt_count = 0;
static int   g_fmt_sel   = 0;
static long g_last_click_ms = 0;    /* double-click detection */
static int  g_last_click_row = -1;

static const char *g_sidebar_labels[] = { "Data","Root","Binaries","Config","Libraries","Repository","System" };
static const char *g_sidebar_paths[]  = { "/data","/","/bin","/etc","/lib","/repo","/system" };
#define SIDEBAR_COUNT 7
static int g_sidebar_sel = 0;

/* Volumes shown in the sidebar, and the flattened Places+Volumes model
 * the listbox actually renders (the toolkit has no tree widget). */
#define VOL_MAX 12
static struct abi_blk_info g_vols[VOL_MAX];
static char g_vol_label_buf[VOL_MAX][48];
static int  g_vol_count = 0;

enum { SIDE_PLACE=0, SIDE_VOLUME=1, SIDE_HEAD=2 };
#define SIDE_MAX (SIDEBAR_COUNT + 1 + VOL_MAX)
static const char *g_side_items[SIDE_MAX];
static int  g_side_kind[SIDE_MAX];
static int  g_side_idx [SIDE_MAX];
static int  g_side_count = 0;
static int  g_side_sel   = 0;


/* ---- path helpers -------------------------------------------------------- */
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
static void set_status2(const char *a,const char *b){
    int p=0; p=str_cat(g_status,p,sizeof(g_status),a);
    str_cat(g_status,p,sizeof(g_status),b);
}

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

/* Explorer-style "Type" column + whether a file is editable text. */
static const char *file_type_name(const struct vfs_dirent_user *e){
    if(e->type==SYS_FS_TYPE_DIR)return "Folder";
    const char *x=find_ext(e->name); if(!x)return "File";
    if(streq(x,".elf"))return "Application";
    if(streq(x,".c")||streq(x,".h"))return "C source";
    if(streq(x,".txt"))return "Text";
    if(streq(x,".md"))return "Markdown";
    if(streq(x,".log"))return "Log";
    if(streq(x,".conf")||streq(x,".toml")||streq(x,".ini"))return "Config";
    if(streq(x,".sh"))return "Script";
    if(streq(x,".py"))return "Python";
    if(streq(x,".tpkg"))return "Package";
    if(streq(x,".so")||streq(x,".a")||streq(x,".o"))return "Library";
    if(streq(x,".tar")||streq(x,".img")||streq(x,".iso"))return "Archive";
    if(streq(x,".bmp")||streq(x,".png")||streq(x,".jpg"))return "Image";
    if(streq(x,".db"))return "Database";
    return "File";
}
static int is_text_file(const struct vfs_dirent_user *e){
    const char *x=find_ext(e->name);
    if(!x)return 1;                    /* extensionless: motd, users, hosts... */
    return streq(x,".txt")||streq(x,".md")||streq(x,".c")||streq(x,".h")||
           streq(x,".conf")||streq(x,".toml")||streq(x,".ini")||streq(x,".log")||
           streq(x,".sh")||streq(x,".py")||streq(x,".json")||streq(x,".csv");
}

/* Full path of the selected row; 0 for ".." / invalid. */
static int selected_full_path(char *out,usize cap){
    if(g_selected<=0)return 0;
    int idx=g_selected-1; if(idx>=g_entry_count)return 0;
    path_join(out,cap,g_path,g_entries[idx].name);
    return 1;
}

/* ---- file operations ----------------------------------------------------- */
static char g_iobuf[32768];

static int file_exists(const char *p){
    long fd=sys_open(p,O_RDONLY,0);
    if(fd<0)return 0;
    sys_close((int)fd);
    return 1;
}

static void do_copy(void){
    if(g_selected==0){ set_status("Cannot copy '..'"); return; }
    int idx=g_selected-1; if(idx>=g_entry_count)return;
    if(g_entries[idx].type!=SYS_FS_TYPE_FILE){ set_status("Only files can be copied (for now)"); return; }
    path_join(g_clipboard,sizeof(g_clipboard),g_path,g_entries[idx].name);
    g_clip_is_file=1;
    set_status2("Copied: ",g_entries[idx].name);
}

/* Windows-style collision handling: "name.txt" -> "name (copy).txt"
 * -> "name (copy 2).txt" ... Returns 0 if no free name was found. */
static int uniquify(char *dst,usize cap,const char *dir,const char *base){
    path_join(dst,cap,dir,base);
    if(!file_exists(dst))return 1;
    const char *x=find_ext(base);
    char stem[SYS_FS_NAME_MAX], ext[16];
    if(x){ usize sl=(usize)(x-base); if(sl>=sizeof(stem))sl=sizeof(stem)-1;
           for(usize i=0;i<sl;i++)stem[i]=base[i]; stem[sl]='\0';
           str_copy(ext,x,sizeof(ext)); }
    else { str_copy(stem,base,sizeof(stem)); ext[0]='\0'; }
    for(int nth=1;nth<=9;nth++){
        char cand[SYS_FS_NAME_MAX]; int p=0;
        p=str_cat(cand,p,sizeof(cand),stem);
        p=str_cat(cand,p,sizeof(cand)," (copy");
        if(nth>1){ char d[2]={(char)('0'+nth),0}; p=str_cat(cand,p,sizeof(cand)," "); p=str_cat(cand,p,sizeof(cand),d); }
        p=str_cat(cand,p,sizeof(cand),")");
        str_cat(cand,p,sizeof(cand),ext);
        path_join(dst,cap,dir,cand);
        if(!file_exists(dst))return 1;
    }
    return 0;
}

static void do_paste(void){
    if(!g_clipboard[0]){ set_status("Clipboard empty -- Copy a file first"); return; }
    if(!g_clip_is_file){ set_status("Can only paste files"); return; }
    long sfd=sys_open(g_clipboard,O_RDONLY,0);
    if(sfd<0){ set_status("Source no longer exists"); return; }
    char dest[256];
    if(!uniquify(dest,sizeof(dest),g_path,path_basename(g_clipboard))){
        sys_close((int)sfd); set_status("Too many copies here already"); return;
    }
    long dfd=sys_open(dest,O_WRONLY|O_CREAT|O_TRUNC,0);
    if(dfd<0){ sys_close((int)sfd); set_status("Create failed (read-only folder?)"); return; }
    /* Chunked fd-to-fd copy: any file size, no truncation. */
    long total=0, err=0;
    for(;;){
        ssize_t n=sys_read_fd((int)sfd,g_iobuf,sizeof(g_iobuf));
        if(n<0){ err=1; break; }
        if(n==0)break;
        ssize_t w=sys_write_to((int)dfd,g_iobuf,(usize)n);
        if(w!=n){ err=1; break; }
        total+=n;
    }
    sys_close((int)sfd); sys_close((int)dfd);
    if(err){ sys_unlink(dest); set_status("Paste failed mid-copy (disk full?)"); return; }
    (void)total;
    set_status2("Pasted: ",path_basename(dest));
}

static void do_copy_path(void){
    char full[256];
    if(g_selected==0){ str_copy(full,g_path,sizeof(full)); }
    else if(!selected_full_path(full,sizeof(full)))return;
    if(sys_clip_copy(full,my_strlen(full))>=0)
        set_status2("Path copied to clipboard: ",full);
    else
        set_status("Clipboard copy failed");
}

static void do_delete(void){
    if(g_selected==0)return; int idx=g_selected-1; if(idx>=g_entry_count)return;
    char full[256]; path_join(full,sizeof(full),g_path,g_entries[idx].name);
    if(sys_unlink(full)==0)set_status2("Deleted: ",g_entries[idx].name);
    else set_status(g_entries[idx].type==SYS_FS_TYPE_DIR
                    ?"Delete failed (folder not empty?)":"Delete failed");
}

static void do_create(int is_dir,const char *name){
    if(!name||!name[0]){ set_status("Name cannot be empty"); return; }
    for(const char *c=name;*c;c++)
        if(*c=='/'){ set_status("Name cannot contain '/'"); return; }
    char full[256]; path_join(full,sizeof(full),g_path,name);
    if(file_exists(full)){ set_status("A file with that name already exists"); return; }
    if(is_dir){
        set_status(sys_mkdir(full,0)==0?"Folder created":"mkdir failed");
    } else {
        long fd=sys_open(full,O_WRONLY|O_CREAT|O_TRUNC,0);
        if(fd<0){ set_status("Create failed (read-only folder?)"); return; }
        sys_close((int)fd);
        set_status2("Created: ",name);
    }
}

static void do_edit(void){
    char full[256];
    if(!selected_full_path(full,sizeof(full))){ set_status("Select a file to edit"); return; }
    int idx=g_selected-1;
    if(g_entries[idx].type==SYS_FS_TYPE_DIR){ set_status("Cannot edit a folder"); return; }
    set_status(sys_exec("/bin/gui_edit",full)==0?"Opened in editor":"Editor failed");
}

static int open_entry(int row){
    if(row==0){ path_pop(); return 1; }
    int idx=row-1; if(idx>=g_entry_count)return 0;
    const struct vfs_dirent_user *e=&g_entries[idx];
    if(e->type==SYS_FS_TYPE_DIR){ char next[256]; path_join(next,sizeof(next),g_path,e->name); str_copy(g_path,next,sizeof(g_path)); return 1; }
    char full[256]; path_join(full,sizeof(full),g_path,e->name);
    const char *ext=find_ext(e->name);
    if(ext&&streq(ext,".elf")){ set_status(sys_exec(full,0)==0?"Launched":"Exec failed"); return 0; }
    if(is_text_file(e)){ set_status(sys_exec("/bin/gui_edit",full)==0?"Opened in editor":"Open failed"); return 0; }
    set_status(sys_exec("/bin/gui_viewer",full)==0?"Opened in viewer":"Open failed"); return 0;
}

/* ---- TobyTK UI ------------------------------------------------------------ */
#define COL_ACCENT 0x004FC3F7u
#define COL_DANGER 0x00EF5350u

static struct tk_window win;
static struct tk_widget *w_table, *w_path, *w_status, *w_list, *w_name;
static int g_base;

static void rebuild(void);

/* file table cell accessor (row 0 = "..") */
static const char *file_cell(void *u,int r,int c){
    static char b[96]; (void)u;
    if(r==0){ if(c==0)return "[^]  .."; if(c==1)return "Up"; return ""; }
    int i=r-1; if(i<0||i>=g_entry_count)return "";
    const struct vfs_dirent_user *e=&g_entries[i];
    if(c==0){
        int p=0;
        b[p++]='['; b[p++]=(e->type==SYS_FS_TYPE_DIR)?'D':' '; b[p++]=']';
        b[p++]=' '; b[p++]=' ';
        for(const char *n=e->name;*n&&p<94;n++)b[p++]=*n; b[p]='\0'; return b;
    }
    if(c==1)return file_type_name(e);
    if(e->type==SYS_FS_TYPE_DIR)return "";
    fmt_size(b,sizeof(b),e->size); return b;
}
static const char *const file_hdr[]={"Name","Type","Size"};
static const int         file_w[]  ={370,110,80};

static void update_view(void){
    if(w_path)   tk_set_text(&win,w_path,g_path);
    if(w_status) tk_set_text(&win,w_status,g_status);
    if(w_table){ w_table->sel=g_selected; tk_table_rows(&win,w_table,g_entry_count+1,file_cell,0); }
    if(w_list && g_confirm_mode==0) w_list->sel=g_side_sel;
    tk_redraw(&win);
}

static void on_up    (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; path_pop(); refresh_listing(); update_view(); }
static void on_open  (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(open_entry(g_selected))refresh_listing(); update_view(); }
static void on_paste (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; do_paste(); refresh_listing(); update_view(); }
static void on_refresh(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; refresh_listing(); update_view(); }
static void on_del   (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(g_selected>0){ g_confirm_mode=1; rebuild(); } }
static void on_newfile(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; g_confirm_mode=3; rebuild(); }
static void on_newdir(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; g_confirm_mode=2; rebuild(); }

/* single click selects; a second click on the same row within 450 ms opens */
static void on_tbl(struct tk_window *w,struct tk_widget *t){
    (void)w;
    int row=tk_table_selected(t);
    long now=sys_clock_ms();
    if(row==g_last_click_row&&now-g_last_click_ms<450){
        g_last_click_row=-1;
        g_selected=row;
        if(open_entry(row))refresh_listing();
        update_view();
        return;
    }
    g_last_click_row=row; g_last_click_ms=now;
    g_selected=row;
}
/* ---- Volumes in the sidebar --------------------------------------------
 *
 * "Format my USB stick" was reachable only from a toolbar button, which is
 * not where anyone looks for it -- you look at the drive. So the Places
 * sidebar grew a Volumes section listing the machine's actual disks, and a
 * right-click on one offers Format.
 *
 * Every disk is listed, including the ones that cannot be formatted. The
 * pick-list deliberately hides those (offering the Windows disk invites
 * someone to try it), but a sidebar is an INVENTORY -- a file manager that
 * silently omits your system disk is lying about the machine. The refusal
 * is explained on right-click instead, which is honest without being an
 * invitation.
 *
 * This still decides nothing about safety: the same kernel guard runs on
 * the request, and Format goes through the same typed confirmation the
 * toolbar path uses. */

/* The one place the "can this be formatted?" filter lives. fmt_scan() uses
 * it too -- two copies of this rule would drift, and the copy that drifted
 * would be the one offering somebody's system disk. */
static int vol_formattable(const struct abi_blk_info *d){
    if(d->class!=1)                                          return 0; /* whole disks */
    if(!(d->flags&ABI_BLK_F_REMOVABLE))                      return 0; /* USB only */
    if(d->flags&(ABI_BLK_F_MOUNTED|ABI_BLK_F_RAM|ABI_BLK_F_GONE)) return 0;
    if(d->fs[0]&&streq(d->fs,"iso9660"))                     return 0; /* boot medium */
    return 1;
}

/* Why not -- said in the words someone would use, not a flag dump. */
static const char *vol_refusal(const struct abi_blk_info *d){
    if(d->flags&ABI_BLK_F_GONE)                return "that device has been unplugged";
    if(d->fs[0]&&streq(d->fs,"iso9660"))       return "this is the live boot medium";
    if(d->flags&ABI_BLK_F_MOUNTED)             return "it is mounted -- unmount it first";
    if(d->flags&ABI_BLK_F_RAM)                 return "it is RAM-backed, not real storage";
    if(!(d->flags&ABI_BLK_F_REMOVABLE))        return "it is a fixed disk -- tobyOS never formats those";
    return "the kernel does not accept this device";
}

static void num_append(char *b,int *p,int cap,unsigned long v){
    char t[24]; int k=0;
    if(!v)t[k++]='0'; else while(v){t[k++]=(char)('0'+v%10);v/=10;}
    while(k>0&&*p+1<cap)b[(*p)++]=t[--k];
    b[*p]='\0';
}

static void vol_scan(void){
    static struct abi_blk_info all[32];
    g_vol_count=0;
    long n=sys_blk_list(all,32);
    if(n<0)return;
    for(long i=0;i<n && g_vol_count<VOL_MAX;i++){
        struct abi_blk_info *d=&all[i];
        if(d->class!=1)continue;                 /* whole disks, not partitions */
        if(d->flags&ABI_BLK_F_GONE)continue;
        g_vols[g_vol_count]=*d;
        {
            char *b=g_vol_label_buf[g_vol_count]; int p=0; b[0]='\0';
            unsigned long mib=(unsigned long)(d->sector_count/2048u);
            p=str_cat(b,p,48,d->name);
            p=str_cat(b,p,48,"  ");
            /* The old label was "name  NNNNN MiB  fs" -- past 17 characters
             * the 150px sidebar clipped it mid-word. */
            if(mib>=1024ul){ num_append(b,&p,48,mib/1024ul); p=str_cat(b,p,48," GB"); }
            else           { num_append(b,&p,48,mib);        p=str_cat(b,p,48," MB"); }
            if(d->flags&ABI_BLK_F_DATA) p=str_cat(b,p,48,"  /data");
            (void)p;
        }
        g_vol_count++;
    }
}

/* The sidebar is Places + a Volumes section, flattened into one listbox
 * (the toolkit has no tree). g_side_kind says what each row is, so a
 * click on the heading does nothing and a click on a volume does not try
 * to cd into a device name. */

static void side_rebuild(void){
    g_side_count=0;
    for(int i=0;i<SIDEBAR_COUNT && g_side_count<SIDE_MAX;i++){
        g_side_items[g_side_count]=g_sidebar_labels[i];
        g_side_kind [g_side_count]=SIDE_PLACE;
        g_side_idx  [g_side_count]=i;
        g_side_count++;
    }
    vol_scan();
    if(g_vol_count>0 && g_side_count<SIDE_MAX){
        g_side_items[g_side_count]="-- Volumes --";
        g_side_kind [g_side_count]=SIDE_HEAD;
        g_side_idx  [g_side_count]=-1;
        g_side_count++;
        for(int i=0;i<g_vol_count && g_side_count<SIDE_MAX;i++){
            g_side_items[g_side_count]=g_vol_label_buf[i];
            g_side_kind [g_side_count]=SIDE_VOLUME;
            g_side_idx  [g_side_count]=i;
            g_side_count++;
        }
    }
    if(g_side_sel>=g_side_count)g_side_sel=0;
}

/* Map the current path back onto a Places row, so the highlight follows
 * navigation the way it did before volumes existed. */
static void side_sync_path(void){
    for(int i=0;i<g_side_count;i++)
        if(g_side_kind[i]==SIDE_PLACE && streq(g_path,g_sidebar_paths[g_side_idx[i]])){
            g_side_sel=i; return;
        }
}

static void vol_describe(const struct abi_blk_info *d){
    char b[96]; int p=0; b[0]='\0';
    p=str_cat(b,p,96,d->name);
    p=str_cat(b,p,96,d->model[0]?"  ":"");
    p=str_cat(b,p,96,d->model[0]?d->model:"");
    if(!vol_formattable(d)){
        p=str_cat(b,p,96,"  -- cannot format: ");
        (void)str_cat(b,p,96,vol_refusal(d));
    } else {
        (void)str_cat(b,p,96,"  -- right-click to format");
    }
    set_status(b);
}

static void on_side(struct tk_window *w,struct tk_widget *l){
    (void)w; int i=tk_selected(l); if(i<0||i>=g_side_count)return;
    g_side_sel=i;
    if(g_side_kind[i]==SIDE_PLACE){
        int k=g_side_idx[i];
        g_sidebar_sel=k; str_copy(g_path,g_sidebar_paths[k],sizeof(g_path));
        refresh_listing(); update_view();
    } else if(g_side_kind[i]==SIDE_VOLUME){
        /* A device is not a directory -- describe it instead of trying to
         * cd into a name that is not a path. */
        vol_describe(&g_vols[g_side_idx[i]]);
        update_view();
    } else {
        update_view();                    /* the heading: inert */
    }
}

/* ---- Format USB: device discovery -------------------------------------- */

/* Build the pick-list. Only devices the kernel would actually accept are
 * offered -- showing the Windows disk greyed out invites someone to try. */
static void fmt_scan(void){
    static struct abi_blk_info all[32];
    g_fmt_count=0; g_fmt_sel=0;
    long n=sys_blk_list(all,32);
    if(n<0)return;
    for(long i=0;i<n && g_fmt_count<FMT_DEVS_MAX;i++){
        struct abi_blk_info *d=&all[i];
        if(!vol_formattable(d))continue;
        g_fmt_devs[g_fmt_count]=*d;
        {
            char *b=g_fmt_label_buf[g_fmt_count];
            int p=0;
            b[0]='\0';
            p=str_cat(b,p,64,d->name);
            p=str_cat(b,p,64,"  ");
            {   /* MiB, decimal, no libc */
                unsigned long v=(unsigned long)(d->sector_count/2048u);
                char t[16]; int k=0;
                if(!v)t[k++]='0'; else while(v){t[k++]=(char)('0'+v%10);v/=10;}
                while(k>0&&p+1<64)b[p++]=t[--k];
                b[p]='\0';
            }
            p=str_cat(b,p,64," MiB  ");
            (void)str_cat(b,p,64,d->fs[0]?d->fs:"(empty)");
            g_fmt_labels[g_fmt_count]=b;
        }
        g_fmt_count++;
    }
}

static void on_fmt_open(struct tk_window *w,struct tk_widget *b){
    (void)w;(void)b;
    fmt_scan();
    if(g_fmt_count==0){
        set_status("No formattable USB found (removable, not mounted, not the boot stick)");
        rebuild();
        return;
    }
    g_confirm_mode=4; rebuild();
}
static void on_fmt_pick(struct tk_window *w,struct tk_widget *b){
    (void)w;(void)b;
    if(g_fmt_count==0)return;
    g_confirm_mode=5; rebuild();
}
static void on_fmt_sel(struct tk_window *w,struct tk_widget *b){
    (void)w; if(b)g_fmt_sel=b->sel;
}

/* The actual request. ERASE because anything reaching this page carries a
 * foreign filesystem or nothing; FORMAT_ONLY because "format my stick" is
 * not "take over my system volume". */
static void on_fmt_go(struct tk_window *w,struct tk_widget *b){
    (void)w;(void)b;
    if(g_fmt_sel<0||g_fmt_sel>=g_fmt_count){ g_confirm_mode=0; rebuild(); return; }
    struct abi_blk_info *d=&g_fmt_devs[g_fmt_sel];

    /* Typed confirmation, same bar as the CLI. */
    const char *typed=w_name?tk_get_text(w_name):"";
    if(!streq(typed,d->name)){
        set_status2("Not formatted -- type the device name exactly: ",d->name);
        g_confirm_mode=0; rebuild(); return;
    }

    struct abi_provision_req req;
    my_memset(&req,0,sizeof req);
    str_copy(req.dev,d->name,sizeof req.dev);
    req.flags=ABI_PROV_F_FORMAT_ONLY|ABI_PROV_F_ERASE;

    long rc=sys_provision(&req);
    if(rc==ABI_PROV_OK_MOUNTED||rc==ABI_PROV_OK_NEXT_BOOT)
        set_status2("Formatted. Mount it with: mount -t tobyfs ",d->name);
    else if(rc==-ABI_EPERM)
        set_status("Refused by the kernel -- not removable, or the boot medium");
    else if(rc==-ABI_EBUSY)
        set_status("Refused -- that device is mounted; unmount it first");
    else
        set_status("Format failed -- the device may be faulty");
    g_confirm_mode=0; refresh_listing(); rebuild();
}

static void on_del_yes(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; do_delete(); g_confirm_mode=0; refresh_listing(); rebuild(); }
static void on_dlg_no (struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; g_confirm_mode=0; set_status("Cancelled"); rebuild(); }
static void on_create (struct tk_window *w,struct tk_widget *b){
    (void)w;(void)b;
    int is_dir=(g_confirm_mode==2);
    const char *nn=w_name?tk_get_text(w_name):0;
    do_create(is_dir,nn);
    g_confirm_mode=0; refresh_listing(); rebuild();
}

/* ---- volume context menu ----------------------------------------------- */
enum { MI_VOL_FORMAT=101, MI_VOL_WHY, MI_VOL_REFRESH };

static const char *const g_volmenu_ok[]  = { "Format as tobyOS volume...","-","Refresh volumes" };
static const int         g_volmenu_ok_id[]={ MI_VOL_FORMAT,0,MI_VOL_REFRESH };
static const char *const g_volmenu_no[]  = { "Why can't I format this?","-","Refresh volumes" };
static const int         g_volmenu_no_id[]={ MI_VOL_WHY,0,MI_VOL_REFRESH };
#define VOLMENU_N 3

/* The right-clicked volume, captured when the menu opened: the listbox
 * selection can move before the menu item fires. */
static int g_vol_ctx = -1;

static void on_vol_menu(struct tk_window *w,int id){
    (void)w;
    if(g_vol_ctx<0||g_vol_ctx>=g_vol_count){ update_view(); return; }
    struct abi_blk_info *d=&g_vols[g_vol_ctx];
    switch(id){
    case MI_VOL_FORMAT: {
        /* Hand off to the EXISTING typed-confirmation page rather than
         * formatting from a menu click. Re-scan first: the pick-list is
         * what on_fmt_go() reads, and it is also a second check that the
         * device is still acceptable. */
        fmt_scan();
        int sel=-1;
        for(int i=0;i<g_fmt_count;i++)
            if(streq(g_fmt_devs[i].name,d->name)){ sel=i; break; }
        if(sel<0){ set_status2("No longer formattable: ",d->name); update_view(); break; }
        g_fmt_sel=sel;
        g_confirm_mode=5;                 /* type the device name to confirm */
        rebuild();
        break;
    }
    case MI_VOL_WHY:
        set_status2("Cannot format -- ",vol_refusal(d));
        update_view();
        break;
    case MI_VOL_REFRESH:
        side_rebuild();
        set_status("Volumes refreshed");
        rebuild();
        break;
    default: update_view(); break;
    }
}

/* ---- context menu ----------------------------------------------------- */
enum { MI_OPEN=1, MI_EDIT, MI_COPY, MI_PASTE, MI_COPYPATH, MI_DELETE };
static const char *const g_menu_items[]={
    "Open","Edit","-","Copy","Paste","Copy file path","-","Delete"
};
static const int g_menu_ids[]={
    MI_OPEN,MI_EDIT,0,MI_COPY,MI_PASTE,MI_COPYPATH,0,MI_DELETE
};
#define MENU_N 8

static void on_menu(struct tk_window *w,int id){
    (void)w;
    switch(id){
    case MI_OPEN:     if(open_entry(g_selected))refresh_listing(); update_view(); break;
    case MI_EDIT:     do_edit(); update_view(); break;
    case MI_COPY:     do_copy(); update_view(); break;
    case MI_PASTE:    do_paste(); refresh_listing(); update_view(); break;
    case MI_COPYPATH: do_copy_path(); update_view(); break;
    case MI_DELETE:   if(g_selected>0){ g_confirm_mode=1; rebuild(); } break;
    default: break;
    }
}

/* Right-click (or the keyboard Menu key): the toolkit already moved the
 * table selection to the row under the cursor and fired on_change, so
 * g_selected is the right-clicked row by the time this runs. */
static void on_context(struct tk_window *w,struct tk_widget *hit,struct tk_event *ev){
    if(g_confirm_mode!=0)return;              /* not on modal pages */
    if(!hit)return;
    if(hit==w_list){
        /* The sidebar. The toolkit already moved the listbox selection to
         * the row under the cursor, so this is the row that was clicked. */
        int i=tk_selected(w_list);
        if(i<0||i>=g_side_count)return;
        if(g_side_kind[i]!=SIDE_VOLUME)return;   /* places have no menu */
        g_side_sel=i;
        g_vol_ctx=g_side_idx[i];
        struct abi_blk_info *d=&g_vols[g_vol_ctx];
        if(vol_formattable(d))
            tk_menu_open(w,ev->x,ev->y,g_volmenu_ok,g_volmenu_ok_id,VOLMENU_N,on_vol_menu);
        else
            tk_menu_open(w,ev->x,ev->y,g_volmenu_no,g_volmenu_no_id,VOLMENU_N,on_vol_menu);
        return;
    }
    if(hit!=w_table)return;                   /* file area otherwise */
    g_selected=tk_table_selected(w_table);
    g_last_click_row=-1;                      /* don't chain into dbl-click */
    tk_menu_open(w,ev->x,ev->y,g_menu_items,g_menu_ids,MENU_N,on_menu);
}

static void tbtn(struct tk_widget *bar,const char *label,tk_cb cb,uint32_t bg){
    struct tk_widget *b=tk_button(&win,bar,label,cb);
    if(bg)tk_colors(b,bg,0x00FFFFFF);
}

static void rebuild(void){
    tk_menu_close(&win);
    tk_clear_children(&win,win.root);
    tk_rewind(&win,g_base);
    win.capture=0; win.focus=0;
    w_table=w_path=w_status=w_list=w_name=0;

    struct tk_widget *root=tk_root(&win);
    tk_pad(root,0); root->gap=0;

    /* toolbar */
    struct tk_widget *tb=tk_hbox(&win,root,4);
    tk_pad(tb,5); tk_colors(tb,win.theme.panel_bg,0);
    tbtn(tb,"Up",on_up,0);
    tbtn(tb,"Open",on_open,0);
    tbtn(tb,"New File",on_newfile,0);
    tbtn(tb,"New Folder",on_newdir,0);
    tbtn(tb,"Paste",on_paste,0);
    tbtn(tb,"Delete",on_del,0x003D2020u);
    tbtn(tb,"Format USB",on_fmt_open,0x003D2020u);
    tbtn(tb,"Refresh",on_refresh,0);

    /* path bar */
    struct tk_widget *pb=tk_hbox(&win,root,6);
    tk_pad(pb,4); tk_colors(pb,0x001A1A2Au,0);
    tk_colors(tk_label(&win,pb,"Path:"),0,win.theme.text_dim);
    w_path=tk_grow(tk_colors(tk_label(&win,pb,g_path),0,COL_ACCENT),1);

    if(g_confirm_mode==1){
        struct tk_widget *dlg=tk_vbox(&win,root,8); tk_pad(dlg,24); tk_grow(dlg,1);
        tk_bold(tk_colors(tk_label(&win,dlg,"Delete this item?"),0,COL_DANGER));
        if(g_selected>0&&g_selected-1<g_entry_count) tk_label(&win,dlg,g_entries[g_selected-1].name);
        struct tk_widget *row=tk_hbox(&win,dlg,8);
        tk_colors(tk_button(&win,row,"Yes, delete",on_del_yes),COL_DANGER,0x00FFFFFF);
        tk_button(&win,row,"Cancel",on_dlg_no);
        tk_grow(tk_label(&win,dlg,""),1);
    } else if(g_confirm_mode==4){
        /* Pick the stick. Only devices the kernel would accept are listed --
         * offering the Windows disk greyed out invites someone to try it. */
        struct tk_widget *dlg=tk_vbox(&win,root,8); tk_pad(dlg,20); tk_grow(dlg,1);
        tk_bold(tk_colors(tk_label(&win,dlg,"Format a USB stick"),0,COL_ACCENT));
        tk_colors(tk_label(&win,dlg,
            "Removable devices that are not mounted and are not the boot medium:"),
            0,win.theme.text_dim);
        w_list=tk_listbox(&win,dlg,g_fmt_labels,g_fmt_count);
        w_list->on_change=on_fmt_sel; w_list->sel=g_fmt_sel; tk_grow(w_list,1);
        struct tk_widget *row=tk_hbox(&win,dlg,8);
        tk_colors(tk_button(&win,row,"Continue",on_fmt_pick),COL_DANGER,0x00FFFFFF);
        tk_button(&win,row,"Cancel",on_dlg_no);
        win.focus=w_list; w_list->focused=1;
    } else if(g_confirm_mode==5){
        /* The typed confirmation. Same bar as the CLI: a mis-click is far
         * likelier here than a mis-typed command, so the friction matters
         * MORE in the GUI, not less. */
        struct tk_widget *dlg=tk_vbox(&win,root,8); tk_pad(dlg,20); tk_grow(dlg,1);
        struct abi_blk_info *d=(g_fmt_sel>=0&&g_fmt_sel<g_fmt_count)
                               ?&g_fmt_devs[g_fmt_sel]:0;
        tk_bold(tk_colors(tk_label(&win,dlg,"EVERYTHING ON THIS DEVICE WILL BE LOST"),
                          0,COL_DANGER));
        if(d){
            tk_label(&win,dlg,g_fmt_labels[g_fmt_sel]);
            if(d->model[0]) tk_colors(tk_label(&win,dlg,d->model),0,win.theme.text_dim);
            tk_colors(tk_label(&win,dlg,
                "Type the device name below to confirm:"),0,win.theme.text_dim);
            tk_colors(tk_label(&win,dlg,d->name),0,COL_ACCENT);
        }
        w_name=tk_field(&win,dlg,"");
        w_name->on_click=on_fmt_go;
        struct tk_widget *row=tk_hbox(&win,dlg,8);
        tk_colors(tk_button(&win,row,"Format",on_fmt_go),COL_DANGER,0x00FFFFFF);
        tk_button(&win,row,"Cancel",on_dlg_no);
        tk_grow(tk_label(&win,dlg,""),1);
        win.focus=w_name; w_name->focused=1;
    } else if(g_confirm_mode==2||g_confirm_mode==3){
        struct tk_widget *dlg=tk_vbox(&win,root,8); tk_pad(dlg,24); tk_grow(dlg,1);
        tk_bold(tk_label(&win,dlg,g_confirm_mode==2?"New folder name:":"New file name:"));
        w_name=tk_field(&win,dlg,"");
        w_name->on_click=on_create;
        struct tk_widget *row=tk_hbox(&win,dlg,8);
        tk_colors(tk_button(&win,row,"Create",on_create),COL_ACCENT,0x00181825u);
        tk_button(&win,row,"Cancel",on_dlg_no);
        tk_grow(tk_label(&win,dlg,""),1);
        win.focus=w_name; w_name->focused=1;
    } else {
        struct tk_widget *main=tk_hbox(&win,root,0); tk_grow(main,1);
        /* places sidebar */
        struct tk_widget *side=tk_vbox(&win,main,2); tk_pad(side,6); tk_size(side,190,0);
        tk_colors(side,0x00252536u,0);
        tk_colors(tk_label(&win,side,"Places"),0,COL_ACCENT);
        side_sync_path();
        w_list=tk_listbox(&win,side,g_side_items,g_side_count);
        w_list->on_change=on_side; w_list->sel=g_side_sel; tk_grow(w_list,1);
        /* file table */
        struct tk_widget *right=tk_vbox(&win,main,0); tk_pad(right,4); tk_grow(right,1);
        w_table=tk_table(&win,right,file_hdr,file_w,3);
        tk_table_rows(&win,w_table,g_entry_count+1,file_cell,0);
        w_table->on_change=on_tbl; w_table->sel=g_selected; tk_grow(w_table,1);
        win.focus=w_table; w_table->focused=1;
    }

    /* status bar */
    struct tk_widget *sb=tk_hbox(&win,root,0); tk_pad(sb,4); tk_colors(sb,win.theme.panel_bg,0);
    w_status=tk_colors(tk_label(&win,sb,g_status),0,win.theme.text_dim);

    tk_redraw(&win);
}

int main(int argc,char **argv);
int main(int argc,char **argv){
    (void)argc;(void)argv;
    if(tk_window_open(&win,780,500,"File Explorer")!=0)return 1;
    tk_on_context(&win,on_context);
    g_base=tk_checkpoint(&win);
    side_rebuild();            /* Places + the machine's actual volumes */
    refresh_listing();
    rebuild();
    return tk_run(&win);
}
