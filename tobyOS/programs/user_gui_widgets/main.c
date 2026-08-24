/* user_gui_widgets/main.c -- /bin/gui_widgets, the Notes application.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The note model is unchanged from
 * the original raw-gui_* version: a text buffer with a line/cursor editor and
 * files under /data/notes/. The UI moved to TobyTK -- the toolbar is real
 * tk_buttons, the file sidebar is a tk_listbox, the status line a tk_label, and
 * the editor pane is a focusable TK_CANVAS that custom-draws the gutter / line
 * numbers / current-line highlight / cursor and receives both mouse and key
 * events through its on_event hook. The rename / delete dialogs are built by
 * rebuilding the window (tk_checkpoint / tk_rewind) for the dialog's lifetime.
 */

#include <toby/tk.h>
#include <unistd.h>

typedef unsigned long      usize;
typedef long               ssize_t;

#define SYS_WRITE           1
#define SYS_CLOSE           4
#define SYS_FS_READDIR     18
#define SYS_FS_READFILE    19
#define SYS_OPEN           35
#define SYS_UNLINK         41
#define SYS_MKDIR          42

#define O_WRONLY    0x1
#define O_CREAT     0x040
#define O_TRUNC     0x200

#define SYS_FS_NAME_MAX    64
#define SYS_FS_TYPE_FILE   1

/* MUST match the kernel ABI struct (name/type/size/uid/gid/mode); omitting the
 * trailing fields makes each entry 12 B short and sys_fs_readdir overruns this
 * array -- see the gui_files fix for the full story. */
struct vfs_dirent_user {
    char     name[SYS_FS_NAME_MAX];
    unsigned type, size, uid, gid, mode;
};

/* arrow / nav key codes posted by the kernel (match the original) */
#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_HOME  0x84
#define KEY_END   0x85

/* ---- file/syscall stubs (kept) --------------------------------------- */
static inline ssize_t sys_write_fd(int fd, const void *b, usize n){ ssize_t r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_WRITE),"D"((long)fd),"S"(b),"d"(n):"rcx","r11","memory"); return r; }
static inline long sys_fs_readdir(const char *p, struct vfs_dirent_user *o, int cap, int off){ long r; register long r10 __asm__("r10")=off; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_FS_READDIR),"D"(p),"S"(o),"d"((long)cap),"r"(r10):"rcx","r11","memory"); return r; }
static inline long sys_fs_readfile(const char *p, void *o, usize cap){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_FS_READFILE),"D"(p),"S"(o),"d"(cap):"rcx","r11","memory"); return r; }
static inline long sys_open(const char *p, int fl, int m){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_OPEN),"D"(p),"S"((long)fl),"d"((long)m):"rcx","r11","memory"); return r; }
static inline long sys_close(int fd){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_CLOSE),"D"((long)fd):"rcx","r11","memory"); return r; }
static inline long sys_unlink(const char *p){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_UNLINK),"D"(p):"rcx","r11","memory"); return r; }
static inline long sys_mkdir(const char *p, int m){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_MKDIR),"D"(p),"S"((long)m):"rcx","r11","memory"); return r; }

/* ---- tiny libc (kept) ------------------------------------------------ */
static void my_memset(void *d,int c,usize n){ unsigned char *p=d; for(usize i=0;i<n;i++)p[i]=(unsigned char)c; }
static void my_memcpy(void *d,const void *s,usize n){ unsigned char *a=d; const unsigned char *b=s; for(usize i=0;i<n;i++)a[i]=b[i]; }
static void my_memmove(void *d,const void *s,usize n){ unsigned char *a=d; const unsigned char *b=s; if(a<b){for(usize i=0;i<n;i++)a[i]=b[i];}else{for(usize i=n;i>0;i--)a[i-1]=b[i-1];} }
static void str_copy(char *d,const char *s,usize cap){ usize i=0; while(i<cap-1&&s[i]){d[i]=s[i];i++;} d[i]='\0'; }
static int streq(const char *a,const char *b){ while(*a&&*a==*b){a++;b++;} return *a=='\0'&&*b=='\0'; }
static void fmt_uint(char *o,unsigned v){ char t[16]; int n=0; if(!v)t[n++]='0'; else while(v){t[n++]=(char)('0'+v%10);v/=10;} int i=0; while(n--)o[i++]=t[n]; o[i]='\0'; }
static int str_append(char *b,int p,const char *s){ while(*s)b[p++]=*s++; return p; }
static int str_append_uint(char *b,int p,unsigned v){ char t[16]; fmt_uint(t,v); return str_append(b,p,t); }

/* ---- editor metrics -------------------------------------------------- */
#define EFONT     14
#define ELINE_H   18
#define GUTTER_W  38
#define TEXT_PAD  6

/* ---- colors ---------------------------------------------------------- */
#define COL_EDITOR      0x001A1A2Au
#define COL_TEXT        0x00E0E0E0u
#define COL_ACCENT      0x004FC3F7u
#define COL_DIM         0x00808890u
#define COL_GUTTER_BG   0x00202030u
#define COL_LINENUM     0x006C7086u
#define COL_CURLINE     0x00252540u
#define COL_CURSOR_BAR  0x004FC3F7u
#define COL_MODIFIED    0x00F38BA8u
#define COL_DANGER      0x00EF5350u

/* ---- state (kept) ---------------------------------------------------- */
#define NOTES_DIR       "/data/notes"
#define MAX_NOTES       64
#define BUF_MAX         8192
#define NAME_MAX_LEN    64

static struct vfs_dirent_user g_notes[MAX_NOTES];
static const char *g_items[MAX_NOTES];
static int  g_note_count = 0;
static int  g_note_sel   = -1;

static char g_buf[BUF_MAX];
static int  g_buf_len     = 0;
static int  g_cursor_x    = 0;
static int  g_cursor_y    = 0;
static int  g_scroll_y    = 0;
static int  g_modified    = 0;
static int  g_total_lines = 1;
static int  g_vis_rows    = 18;

static char g_filename[NAME_MAX_LEN] = "";
static int  g_dialog_mode = 0;     /* 0=editor, 1=rename, 2=delete-confirm */
static int  g_untitled_n  = 1;

/* ---- text buffer helpers (kept) -------------------------------------- */
static int line_start(int line){ int off=0; for(int l=0;l<line&&off<g_buf_len;off++) if(g_buf[off]=='\n')l++; return off; }
static int line_len(int line){ int off=line_start(line),len=0; while(off+len<g_buf_len&&g_buf[off+len]!='\n')len++; return len; }
static void recount_lines(void){ g_total_lines=1; for(int i=0;i<g_buf_len;i++) if(g_buf[i]=='\n')g_total_lines++; }
static int cursor_offset(void){ return line_start(g_cursor_y)+g_cursor_x; }
static void clamp_cursor(void){ if(g_cursor_y>=g_total_lines)g_cursor_y=g_total_lines-1; if(g_cursor_y<0)g_cursor_y=0; int len=line_len(g_cursor_y); if(g_cursor_x>len)g_cursor_x=len; if(g_cursor_x<0)g_cursor_x=0; }
static void ensure_visible(void){ if(g_cursor_y<g_scroll_y)g_scroll_y=g_cursor_y; if(g_cursor_y>=g_scroll_y+g_vis_rows)g_scroll_y=g_cursor_y-g_vis_rows+1; if(g_scroll_y<0)g_scroll_y=0; }

static void buf_insert(int off,char ch){ if(g_buf_len>=BUF_MAX-1)return; my_memmove(g_buf+off+1,g_buf+off,(usize)(g_buf_len-off)); g_buf[off]=ch; g_buf_len++; g_buf[g_buf_len]='\0'; g_modified=1; recount_lines(); }
static void buf_delete(int off){ if(off<0||off>=g_buf_len)return; my_memmove(g_buf+off,g_buf+off+1,(usize)(g_buf_len-off-1)); g_buf_len--; g_buf[g_buf_len]='\0'; g_modified=1; recount_lines(); }

/* ---- file operations (kept) ------------------------------------------ */
static void build_path(char *out,usize cap,const char *name){ int p=0; const char *d=NOTES_DIR; while(*d&&(usize)p+1<cap)out[p++]=*d++; out[p++]='/'; while(*name&&(usize)p+1<cap)out[p++]=*name++; out[p]='\0'; }

static void refresh_notes(void){
    my_memset(g_notes,0,sizeof(g_notes));
    g_note_count=0;
    long n=sys_fs_readdir(NOTES_DIR,g_notes,MAX_NOTES,0);
    if(n<=0){ g_note_count=0; return; }
    int wc=0;
    for(int i=0;i<(int)n;i++) if(g_notes[i].type==SYS_FS_TYPE_FILE){ if(wc!=i)my_memcpy(&g_notes[wc],&g_notes[i],sizeof(g_notes[0])); wc++; }
    g_note_count=wc;
    for(int i=1;i<g_note_count;i++){
        struct vfs_dirent_user tmp; my_memcpy(&tmp,&g_notes[i],sizeof(tmp));
        int j=i-1;
        while(j>=0){ const char *a=g_notes[j].name,*b=tmp.name; while(*a&&*b&&*a==*b){a++;b++;} if((int)(unsigned char)*a-(int)(unsigned char)*b<=0)break; my_memcpy(&g_notes[j+1],&g_notes[j],sizeof(tmp)); j--; }
        my_memcpy(&g_notes[j+1],&tmp,sizeof(tmp));
    }
}

static void load_note(int idx){
    if(idx<0||idx>=g_note_count)return;
    char path[128]; build_path(path,sizeof(path),g_notes[idx].name);
    long n=sys_fs_readfile(path,g_buf,BUF_MAX-1); if(n<0)n=0;
    g_buf_len=(int)n; g_buf[g_buf_len]='\0';
    g_cursor_x=g_cursor_y=g_scroll_y=0; g_modified=0;
    str_copy(g_filename,g_notes[idx].name,NAME_MAX_LEN);
    g_note_sel=idx; recount_lines();
}

static void save_note(void){
    if(!g_filename[0])return;
    char path[128]; build_path(path,sizeof(path),g_filename);
    long fd=sys_open(path,O_WRONLY|O_CREAT|O_TRUNC,0); if(fd<0)return;
    if(g_buf_len>0)sys_write_fd((int)fd,g_buf,(usize)g_buf_len);
    sys_close((int)fd); g_modified=0;
    refresh_notes();
    for(int i=0;i<g_note_count;i++) if(streq(g_notes[i].name,g_filename)){ g_note_sel=i; break; }
}

static void new_note(void){
    for(;;){
        char name[NAME_MAX_LEN]; int p=0;
        p=str_append(name,p,"untitled_"); p=str_append_uint(name,p,(unsigned)g_untitled_n); p=str_append(name,p,".txt"); name[p]='\0';
        int exists=0; for(int i=0;i<g_note_count;i++) if(streq(g_notes[i].name,name)){exists=1;break;}
        if(!exists){ str_copy(g_filename,name,NAME_MAX_LEN); g_untitled_n++; break; }
        g_untitled_n++;
    }
    g_buf[0]='\0'; g_buf_len=0; g_cursor_x=g_cursor_y=g_scroll_y=0; g_total_lines=1; g_modified=0;
    save_note();
}

static void delete_note(void){
    if(!g_filename[0])return;
    char path[128]; build_path(path,sizeof(path),g_filename); sys_unlink(path);
    refresh_notes();
    if(g_note_count>0){ if(g_note_sel>=g_note_count)g_note_sel=g_note_count-1; load_note(g_note_sel); }
    else { g_note_sel=-1; g_filename[0]='\0'; g_buf_len=0; g_buf[0]='\0'; g_cursor_x=g_cursor_y=g_scroll_y=0; g_modified=0; g_total_lines=1; }
}

static void do_rename(const char *new_name){
    if(!g_filename[0]||!new_name[0])return;
    if(streq(g_filename,new_name))return;
    char op[128],np[128]; build_path(op,sizeof(op),g_filename); build_path(np,sizeof(np),new_name);
    long fd=sys_open(np,O_WRONLY|O_CREAT|O_TRUNC,0); if(fd<0)return;
    if(g_buf_len>0)sys_write_fd((int)fd,g_buf,(usize)g_buf_len);
    sys_close((int)fd); sys_unlink(op);
    str_copy(g_filename,new_name,NAME_MAX_LEN); g_modified=0;
    refresh_notes();
    for(int i=0;i<g_note_count;i++) if(streq(g_notes[i].name,g_filename)){ g_note_sel=i; break; }
}

/* ---- TobyTK UI ------------------------------------------------------- */
static struct tk_window win;
static struct tk_widget *w_editor, *w_status, *w_fname, *w_list, *w_rename;
static int g_base;

static void rebuild(void);

static void update_status(void){
    char st[128]; int sp=0;
    sp=str_append(st,sp," Line "); sp=str_append_uint(st,sp,(unsigned)(g_cursor_y+1));
    sp=str_append(st,sp,", Col "); sp=str_append_uint(st,sp,(unsigned)(g_cursor_x+1));
    if(g_modified) sp=str_append(st,sp,"  |  Modified");
    sp=str_append(st,sp,"  |  "); sp=str_append_uint(st,sp,(unsigned)g_note_count); sp=str_append(st,sp," notes");
    st[sp]='\0';
    if(w_status){ tk_colors(w_status,0,g_modified?COL_MODIFIED:COL_DIM); tk_set_text(&win,w_status,st); }
}

/* map a click x (canvas-local, past the gutter) to a column on `line` */
static int col_at_x(int line,int px){
    int ls=line_start(line),ll=line_len(line);
    char tmp[256]; int prev=0;
    for(int i=1;i<=ll&&i<250;i++){
        my_memcpy(tmp,g_buf+ls,(usize)i); tmp[i]='\0';
        int w=tk_text_width(tmp,EFONT,0);
        if((prev+w)/2>px) return i-1;
        prev=w;
    }
    return ll;
}

static void editor_paint(struct tk_window *w,struct tk_widget *c){
    int x,y,cw,ch; tk_geom(c,&x,&y,&cw,&ch);
    if(cw<=0||ch<=0)return;
    g_vis_rows=ch/ELINE_H; if(g_vis_rows<1)g_vis_rows=1;
    tk_draw_fill(w,x,y,cw,ch,COL_EDITOR);
    tk_draw_fill(w,x,y,GUTTER_W,ch,COL_GUTTER_BG);
    tk_draw_fill(w,x+GUTTER_W,y,1,ch,0x005D5D6Du);   /* gutter separator (1px fill rect) */

    if(!g_filename[0]){
        tk_draw_text(w,x+GUTTER_W+24,y+50,"Select or create a note",COL_DIM,EFONT,0);
        return;
    }

    for(int vi=0;vi<g_vis_rows;vi++){
        int row=g_scroll_y+vi; if(row>=g_total_lines)break;
        int ry=y+vi*ELINE_H; int is_cur=(row==g_cursor_y);
        if(is_cur){ tk_draw_fill(w,x,ry,GUTTER_W,ELINE_H,COL_CURLINE); tk_draw_fill(w,x+GUTTER_W+1,ry,cw-GUTTER_W-1,ELINE_H,COL_CURLINE); }
        char lnum[8]; fmt_uint(lnum,(unsigned)(row+1));
        int lw=tk_text_width(lnum,EFONT,0);
        tk_draw_text(w,x+GUTTER_W-lw-6,ry+2,lnum,is_cur?COL_ACCENT:COL_LINENUM,EFONT,0);
        int ls=line_start(row),ll=line_len(row);
        if(ll>0){ char ld[256]; int dl=ll<250?ll:250; my_memcpy(ld,g_buf+ls,(usize)dl); ld[dl]='\0';
                  tk_draw_text(w,x+GUTTER_W+TEXT_PAD,ry+2,ld,COL_TEXT,EFONT,0); }
        if(is_cur){
            char pre[256]; int pl=g_cursor_x<250?g_cursor_x:250; my_memcpy(pre,g_buf+ls,(usize)pl); pre[pl]='\0';
            int cxp=tk_text_width(pre,EFONT,0);
            tk_draw_fill(w,x+GUTTER_W+TEXT_PAD+cxp,ry+1,2,EFONT+2,COL_CURSOR_BAR);
        }
    }

    if(g_total_lines>g_vis_rows){
        int sbx=x+cw-5; tk_draw_fill(w,sbx,y,4,ch,0x00252536u);
        int th=(g_vis_rows*ch)/g_total_lines; if(th<8)th=8;
        int maxsc=g_total_lines-g_vis_rows; int ty=y;
        if(maxsc>0)ty+=(g_scroll_y*(ch-th))/maxsc;
        tk_draw_fill(w,sbx,ty,4,th,COL_ACCENT);
    }
}

static void editor_key(unsigned char k){
    if(k==KEY_UP){ if(g_cursor_y>0){g_cursor_y--;clamp_cursor();ensure_visible();} }
    else if(k==KEY_DOWN){ if(g_cursor_y<g_total_lines-1){g_cursor_y++;clamp_cursor();ensure_visible();} }
    else if(k==KEY_LEFT){ if(g_cursor_x>0)g_cursor_x--; else if(g_cursor_y>0){g_cursor_y--;g_cursor_x=line_len(g_cursor_y);ensure_visible();} }
    else if(k==KEY_RIGHT){ int ll=line_len(g_cursor_y); if(g_cursor_x<ll)g_cursor_x++; else if(g_cursor_y<g_total_lines-1){g_cursor_y++;g_cursor_x=0;ensure_visible();} }
    else if(k==KEY_HOME){ g_cursor_x=0; }
    else if(k==KEY_END){ g_cursor_x=line_len(g_cursor_y); }
    else if(k=='\b'||k==127){ int off=cursor_offset(); if(off>0){ if(g_cursor_x>0)g_cursor_x--; else if(g_cursor_y>0){g_cursor_y--;g_cursor_x=line_len(g_cursor_y);} buf_delete(off-1); ensure_visible(); } }
    else if(k=='\n'||k=='\r'){ int off=cursor_offset(); buf_insert(off,'\n'); g_cursor_y++; g_cursor_x=0; ensure_visible(); }
    else if(k>=0x20&&k<=0x7E){ int off=cursor_offset(); buf_insert(off,(char)k); g_cursor_x++; }
}

static void editor_event(struct tk_window *w,struct tk_widget *c,struct tk_event *ev){
    if(!g_filename[0])return;
    if(ev->type==TK_EV_MOUSE_DOWN){
        int x,y,cw,ch; tk_geom(c,&x,&y,&cw,&ch); (void)cw; (void)ch;
        int vy=(ev->y-y)/ELINE_H; int row=g_scroll_y+vy;
        if(row>=g_total_lines)row=g_total_lines-1; if(row<0)row=0;
        g_cursor_y=row;
        int px=ev->x-(x+GUTTER_W+TEXT_PAD); if(px<0)px=0;
        g_cursor_x=col_at_x(row,px);
        update_status(); tk_redraw(w);
    } else if(ev->type==TK_EV_KEY){
        editor_key(ev->key); update_status(); tk_redraw(w);
    } else if(ev->type==TK_EV_WHEEL&&ev->wheel){
        /* View-only scroll; the caret stays put until a key moves it
         * (which then calls ensure_visible and snaps back, as expected). */
        g_scroll_y-=(int)ev->wheel*TK_WHEEL_LINES;
        int maxs=g_total_lines-g_vis_rows; if(maxs<0)maxs=0;
        if(g_scroll_y>maxs)g_scroll_y=maxs;
        if(g_scroll_y<0)g_scroll_y=0;
        tk_redraw(w);
    }
}

/* ---- toolbar / sidebar callbacks ------------------------------------- */
static void on_pick(struct tk_window *w,struct tk_widget *l){
    int i=tk_selected(l); if(i<0||i>=g_note_count)return;
    load_note(i);
    tk_set_text(w,w_fname,g_filename);
    update_status();
    win.focus=w_editor; if(w_editor)w_editor->focused=1;
    tk_redraw(w);
}
static void on_new(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; new_note(); rebuild(); }
static void on_save(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; save_note(); rebuild(); }
static void on_delete(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(g_filename[0]){g_dialog_mode=2;rebuild();} }
static void on_rename(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; if(g_filename[0]){g_dialog_mode=1;rebuild();} }

static void on_rn_ok(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; const char *nn=w_rename?tk_get_text(w_rename):0; if(nn&&nn[0])do_rename(nn); g_dialog_mode=0; rebuild(); }
static void on_rn_cancel(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; g_dialog_mode=0; rebuild(); }
static void on_del_yes(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; delete_note(); g_dialog_mode=0; rebuild(); }
static void on_del_no(struct tk_window *w,struct tk_widget *b){ (void)w;(void)b; g_dialog_mode=0; rebuild(); }

static void rebuild(void){
    tk_clear_children(&win,win.root);
    tk_rewind(&win,g_base);
    win.capture=0; win.focus=0;
    w_editor=w_status=w_fname=w_list=w_rename=0;

    struct tk_widget *root=tk_root(&win);
    tk_pad(root,0); root->gap=0;

    /* toolbar */
    struct tk_widget *tb=tk_hbox(&win,root,6);
    tk_pad(tb,6); tk_colors(tb,win.theme.panel_bg,0);
    tk_button(&win,tb,"New",on_new);
    tk_button(&win,tb,"Save",on_save);
    tk_colors(tk_button(&win,tb,"Delete",on_delete),0x003D2020u,COL_TEXT);
    tk_button(&win,tb,"Rename",on_rename);
    w_fname=tk_align(tk_grow(tk_colors(tk_label(&win,tb,g_filename),0,COL_ACCENT),1),TK_ALIGN_RIGHT);

    if(g_dialog_mode==1){
        struct tk_widget *dlg=tk_vbox(&win,root,8); tk_pad(dlg,24); tk_grow(dlg,1);
        tk_bold(tk_label(&win,dlg,"Rename note"));
        tk_colors(tk_label(&win,dlg,"New name:"),0,COL_DIM);
        w_rename=tk_field(&win,dlg,g_filename);
        w_rename->on_click=on_rn_ok;
        struct tk_widget *row=tk_hbox(&win,dlg,8);
        tk_colors(tk_button(&win,row,"OK",on_rn_ok),COL_ACCENT,0x00181825u);
        tk_button(&win,row,"Cancel",on_rn_cancel);
        tk_grow(tk_label(&win,dlg,""),1);
        win.focus=w_rename; w_rename->focused=1;
    } else if(g_dialog_mode==2){
        struct tk_widget *dlg=tk_vbox(&win,root,8); tk_pad(dlg,24); tk_grow(dlg,1);
        tk_bold(tk_colors(tk_label(&win,dlg,"Delete this note?"),0,COL_DANGER));
        tk_label(&win,dlg,g_filename);
        struct tk_widget *row=tk_hbox(&win,dlg,8);
        tk_colors(tk_button(&win,row,"Yes, delete",on_del_yes),COL_DANGER,0x00FFFFFFu);
        tk_button(&win,row,"Cancel",on_del_no);
        tk_grow(tk_label(&win,dlg,""),1);
    } else {
        /* main: sidebar + editor */
        struct tk_widget *main=tk_hbox(&win,root,0); tk_grow(main,1);
        int n=0; for(int i=0;i<g_note_count;i++) g_items[n++]=g_notes[i].name;
        w_list=tk_listbox(&win,main,g_items,n);
        tk_size(w_list,160,0);
        w_list->on_change=on_pick;
        if(g_note_sel>=0&&g_note_sel<n)w_list->sel=g_note_sel;
        w_editor=tk_canvas(&win,main,editor_paint);
        tk_grow(w_editor,1);
        tk_on_event(w_editor,editor_event);
        win.focus=w_editor; w_editor->focused=1;
    }

    /* status bar */
    struct tk_widget *sb=tk_hbox(&win,root,0); tk_pad(sb,4); tk_colors(sb,win.theme.panel_bg,0);
    w_status=tk_colors(tk_label(&win,sb,""),0,COL_DIM);
    update_status();

    tk_redraw(&win);
}

int main(int argc,char **argv);
int main(int argc,char **argv){
    (void)argc;(void)argv;
    sys_mkdir("/data",0);
    sys_mkdir(NOTES_DIR,0);

    if(tk_window_open(&win,640,440,"Notes")!=0)return 1;
    g_base=tk_checkpoint(&win);

    refresh_notes();
    if(g_note_count>0)load_note(0);

    rebuild();
    return tk_run(&win);
}
