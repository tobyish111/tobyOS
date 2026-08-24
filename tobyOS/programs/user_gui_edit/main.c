/* user_gui_edit/main.c -- /bin/gui_edit (TobyEdit), a simple GUI text editor.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The editor model is unchanged: a
 * line array with insert/delete/split, arrow + Home/End navigation, scrolling,
 * line numbers, and Ctrl+S save / Ctrl+Q quit / Ctrl+N new. The UI moved to
 * TobyTK: the whole editor is a full-window TK_CANVAS drawn with the monospace
 * tk_draw_text_mono (8px cells, so the cursor at col*8 stays aligned), and keys
 * flow through the window key hook. Opens argv[1] if given.
 */

#include <toby/tk.h>
#include <unistd.h>

#define SYS_WRITE 1
#define SYS_READ  2
#define SYS_CLOSE 4
#define SYS_OPEN  35
#define SYS_CLOCK_MS 48
#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_CREAT  0x040
#define O_TRUNC  0x200

static inline long sys_write_fd(int fd,const void *b,unsigned long n){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_WRITE),"D"((long)fd),"S"(b),"d"(n):"rcx","r11","memory"); return r; }
static inline long sys_read(int fd,void *b,unsigned long n){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_READ),"D"((long)fd),"S"(b),"d"(n):"rcx","r11","memory"); return r; }
static inline long sys_open(const char *p,int fl,int m){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_OPEN),"D"(p),"S"((long)fl),"d"((long)m):"rcx","r11","memory"); return r; }
static inline long sys_close(int fd){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_CLOSE),"D"((long)fd):"rcx","r11","memory"); return r; }
static inline long sys_clock_ms(void){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_CLOCK_MS):"rcx","r11","memory"); return r; }

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_HOME 0x84
#define KEY_END 0x85

/* ---- tiny helpers ---------------------------------------------------- */
static unsigned long my_strlen(const char *s){ const char *p=s; while(*p)p++; return (unsigned long)(p-s); }
static void my_memset(void *d,int c,unsigned long n){ unsigned char *p=d; for(unsigned long i=0;i<n;i++)p[i]=(unsigned char)c; }
static void my_memcpy(void *d,const void *s,unsigned long n){ unsigned char *a=d; const unsigned char *b=s; for(unsigned long i=0;i<n;i++)a[i]=b[i]; }
static void my_strcpy(char *d,const char *s){ while(*s)*d++=*s++; *d='\0'; }
static void my_strncpy(char *d,const char *s,unsigned long cap){ unsigned long i=0; while(i<cap-1&&s[i]){d[i]=s[i];i++;} d[i]='\0'; }
static void fmt_uint(char *o,unsigned v){ char t[16]; int n=0; if(!v)t[n++]='0'; else while(v){t[n++]=(char)('0'+v%10);v/=10;} int i=0; while(n--)o[i++]=t[n]; o[i]='\0'; }
static int str_append(char *b,int p,const char *s){ while(*s)b[p++]=*s++; return p; }
static int str_append_uint(char *b,int p,unsigned v){ char t[16]; fmt_uint(t,v); return str_append(b,p,t); }

/* ---- layout ---------------------------------------------------------- */
#define WIN_W 640
#define WIN_H 480
#define TITLE_H 20
#define EDITOR_Y TITLE_H
#define STATUS_H 20
#define EDITOR_H (WIN_H-TITLE_H-STATUS_H)
#define STATUS_Y (WIN_H-STATUS_H)
#define CELL_W 8
#define CELL_H 12
#define LINE_H 14
#define GUTTER_W 40
#define TEXT_X (GUTTER_W+4)
#define VISIBLE_ROWS (EDITOR_H/LINE_H)
#define VISIBLE_COLS ((WIN_W-TEXT_X-4)/CELL_W)

#define COL_BG       0x001E1E2Eu
#define COL_GUTTER   0x00181825u
#define COL_LINENUM  0x006C7086u
#define COL_TEXT     0x00CDD6F4u
#define COL_CURSOR   0x00313244u
#define COL_STATUS   0x00181825u
#define COL_ST_TEXT  0x00A6ADC8u
#define COL_MODIFIED 0x00F38BA8u
#define COL_TITLE_BG 0x00181825u
#define COL_TITLE_FG 0x00CDD6F4u
#define COL_ACCENT   0x0089B4FAu
#define COL_MSG_OK   0x00A6E3A1u
#define COL_MSG_ERR  0x00F38BA8u

/* ---- text buffer (unchanged model) ----------------------------------- */
#define MAX_LINES 512
#define MAX_COLS 256
static char g_lines[MAX_LINES][MAX_COLS];
static int g_line_count=1, g_cursor_row, g_cursor_col, g_scroll_row, g_modified;
static char g_filename[256]; static int g_has_file;
static char g_message[64]; static uint32_t g_msg_color; static long g_msg_expire;

static int line_len(int row){ return (int)my_strlen(g_lines[row]); }
static void set_message(const char *msg,uint32_t color){ my_strncpy(g_message,msg,sizeof(g_message)); g_msg_color=color; g_msg_expire=sys_clock_ms()+3000; }

static void load_file(const char *path){
    long ffd=sys_open(path,O_RDONLY,0); if(ffd<0){ set_message("Open failed",COL_MSG_ERR); return; }
    static char filebuf[MAX_LINES*80]; long total=0;
    for(;;){ long n=sys_read((int)ffd,filebuf+total,sizeof(filebuf)-(unsigned long)total-1); if(n<=0)break; total+=n; if((unsigned long)total>=sizeof(filebuf)-1)break; }
    sys_close((int)ffd); filebuf[total]='\0';
    my_memset(g_lines,0,sizeof(g_lines)); g_line_count=0; g_cursor_row=g_cursor_col=g_scroll_row=0;
    int col=0;
    for(long i=0;i<total&&g_line_count<MAX_LINES;i++){
        if(filebuf[i]=='\n'){ g_lines[g_line_count][col]='\0'; g_line_count++; col=0; }
        else if(filebuf[i]=='\r')continue;
        else { if(col<MAX_COLS-1)g_lines[g_line_count][col++]=filebuf[i]; }
    }
    if(col>0||g_line_count==0){ g_lines[g_line_count][col]='\0'; g_line_count++; }
    if(g_line_count==0)g_line_count=1;
    g_modified=0; my_strcpy(g_filename,path); g_has_file=1; set_message("Loaded",COL_MSG_OK);
}
static void save_file(void){
    if(!g_has_file){ set_message("No filename",COL_MSG_ERR); return; }
    long ffd=sys_open(g_filename,O_WRONLY|O_CREAT|O_TRUNC,0); if(ffd<0){ set_message("Save failed",COL_MSG_ERR); return; }
    for(int i=0;i<g_line_count;i++){ int len=line_len(i); if(len>0)sys_write_fd((int)ffd,g_lines[i],(unsigned long)len); if(i<g_line_count-1)sys_write_fd((int)ffd,"\n",1); }
    sys_close((int)ffd); g_modified=0; set_message("Saved",COL_MSG_OK);
}
static void new_file(void){ my_memset(g_lines,0,sizeof(g_lines)); g_line_count=1; g_cursor_row=g_cursor_col=g_scroll_row=0; g_modified=0; g_has_file=0; g_filename[0]='\0'; set_message("New file",COL_MSG_OK); }

static void ensure_cursor_visible(void){
    if(g_cursor_row<g_scroll_row)g_scroll_row=g_cursor_row;
    if(g_cursor_row>=g_scroll_row+VISIBLE_ROWS)g_scroll_row=g_cursor_row-VISIBLE_ROWS+1;
    if(g_scroll_row<0)g_scroll_row=0;
}
/* Wheel scrolls the view WITHOUT moving the caret -- ensure_cursor_visible
 * only runs on cursor motion, so the view stays where the wheel left it
 * until you actually type or arrow somewhere. */
static void on_event(struct tk_window *w,struct tk_widget *c,struct tk_event *ev){
    (void)c;
    if(ev->type!=TK_EV_WHEEL||!ev->wheel)return;
    g_scroll_row-=(int)ev->wheel*TK_WHEEL_LINES;
    int maxs=g_line_count-VISIBLE_ROWS; if(maxs<0)maxs=0;
    if(g_scroll_row>maxs)g_scroll_row=maxs;
    if(g_scroll_row<0)g_scroll_row=0;
    tk_redraw(w);
}

static void insert_char(char ch){ int len=line_len(g_cursor_row); if(len>=MAX_COLS-2)return; for(int i=len;i>=g_cursor_col;i--)g_lines[g_cursor_row][i+1]=g_lines[g_cursor_row][i]; g_lines[g_cursor_row][g_cursor_col]=ch; g_cursor_col++; g_modified=1; }
static void delete_back(void){
    if(g_cursor_col>0){ int len=line_len(g_cursor_row); for(int i=g_cursor_col-1;i<len;i++)g_lines[g_cursor_row][i]=g_lines[g_cursor_row][i+1]; g_cursor_col--; g_modified=1; }
    else if(g_cursor_row>0){
        int prev_len=line_len(g_cursor_row-1), cur_len=line_len(g_cursor_row);
        if(prev_len+cur_len<MAX_COLS-1)my_memcpy(g_lines[g_cursor_row-1]+prev_len,g_lines[g_cursor_row],(unsigned long)(cur_len+1));
        for(int i=g_cursor_row;i<g_line_count-1;i++)my_memcpy(g_lines[i],g_lines[i+1],MAX_COLS);
        my_memset(g_lines[g_line_count-1],0,MAX_COLS); g_line_count--; g_cursor_row--; g_cursor_col=prev_len; g_modified=1; ensure_cursor_visible();
    }
}
static void split_line(void){
    if(g_line_count>=MAX_LINES)return;
    for(int i=g_line_count;i>g_cursor_row+1;i--)my_memcpy(g_lines[i],g_lines[i-1],MAX_COLS);
    g_line_count++;
    int len=line_len(g_cursor_row), tail=len-g_cursor_col;
    if(tail>0)my_memcpy(g_lines[g_cursor_row+1],g_lines[g_cursor_row]+g_cursor_col,(unsigned long)(tail+1));
    else g_lines[g_cursor_row+1][0]='\0';
    g_lines[g_cursor_row][g_cursor_col]='\0';
    g_cursor_row++; g_cursor_col=0; g_modified=1; ensure_cursor_visible();
}

/* ---- TobyTK canvas --------------------------------------------------- */
static struct tk_window win;

static void paint(struct tk_window *w,struct tk_widget *c){
    (void)c;
    tk_draw_fill(w,0,0,WIN_W,WIN_H,COL_BG);
    tk_draw_fill(w,0,0,WIN_W,TITLE_H,COL_TITLE_BG);
    tk_draw_fill(w,0,TITLE_H-1,WIN_W,1,COL_ACCENT);
    char title[80]; int tp=0;
    tp=str_append(title,tp,"TobyEdit");
    if(g_has_file){ tp=str_append(title,tp," - "); tp=str_append(title,tp,g_filename); } else tp=str_append(title,tp," - [untitled]");
    if(g_modified)tp=str_append(title,tp," *");
    title[tp]='\0';
    tk_draw_text_mono(w,8,5,title,g_modified?COL_MODIFIED:COL_TITLE_FG,COL_TITLE_BG);

    tk_draw_fill(w,0,EDITOR_Y,GUTTER_W,EDITOR_H,COL_GUTTER);
    tk_draw_fill(w,GUTTER_W,EDITOR_Y,1,EDITOR_H,0x00313244u);

    for(int vi=0;vi<VISIBLE_ROWS;vi++){
        int row=g_scroll_row+vi; if(row>=g_line_count)break;
        int y=EDITOR_Y+vi*LINE_H; int is_cur=(row==g_cursor_row);
        if(is_cur){ tk_draw_fill(w,GUTTER_W+1,y,WIN_W-GUTTER_W-1,LINE_H,COL_CURSOR); tk_draw_fill(w,0,y,GUTTER_W,LINE_H,COL_CURSOR); }
        uint32_t gbg=is_cur?COL_CURSOR:COL_GUTTER;
        char lnum[8]; fmt_uint(lnum,(unsigned)(row+1));
        int lnum_len=(int)my_strlen(lnum); int lnum_x=GUTTER_W-(lnum_len+1)*CELL_W; if(lnum_x<2)lnum_x=2;
        tk_draw_text_mono(w,lnum_x,y+1,lnum,is_cur?COL_ACCENT:COL_LINENUM,gbg);
        int len=line_len(row);
        if(len>0){ char disp[VISIBLE_COLS+2]; int dlen=len; if(dlen>VISIBLE_COLS)dlen=VISIBLE_COLS;
            if(dlen>0){ my_memcpy(disp,g_lines[row],(unsigned long)dlen); disp[dlen]='\0';
                        tk_draw_text_mono(w,TEXT_X,y+1,disp,COL_TEXT,is_cur?COL_CURSOR:COL_BG); } }
        if(is_cur){ int cx=TEXT_X+g_cursor_col*CELL_W; if(cx<WIN_W-4)tk_draw_fill(w,cx,y+1,2,CELL_H,COL_ACCENT); }
    }

    tk_draw_fill(w,0,STATUS_Y,WIN_W,STATUS_H,COL_STATUS);
    tk_draw_fill(w,0,STATUS_Y,WIN_W,1,0x00313244u);
    char st[128]; int sp=0;
    sp=str_append(st,sp," Ln "); sp=str_append_uint(st,sp,(unsigned)(g_cursor_row+1));
    sp=str_append(st,sp,", Col "); sp=str_append_uint(st,sp,(unsigned)(g_cursor_col+1));
    sp=str_append(st,sp,"  |  "); sp=str_append(st,sp,g_has_file?g_filename:"[untitled]");
    if(g_modified)sp=str_append(st,sp,"  [Modified]");
    sp=str_append(st,sp,"  |  "); sp=str_append_uint(st,sp,(unsigned)g_line_count); sp=str_append(st,sp," lines");
    st[sp]='\0';
    tk_draw_text_mono(w,4,STATUS_Y+5,st,COL_ST_TEXT,COL_STATUS);
    if(g_message[0]&&sys_clock_ms()<g_msg_expire){ int mx=WIN_W-((int)my_strlen(g_message)+1)*CELL_W; tk_draw_text_mono(w,mx,STATUS_Y+5,g_message,g_msg_color,COL_STATUS); }

    if(g_line_count>VISIBLE_ROWS){
        int sb_x=WIN_W-6; tk_draw_fill(w,sb_x,EDITOR_Y,4,EDITOR_H,0x00252536u);
        int th=(VISIBLE_ROWS*EDITOR_H)/g_line_count; if(th<8)th=8;
        int maxsc=g_line_count-VISIBLE_ROWS; int ty=EDITOR_Y; if(maxsc>0)ty+=(g_scroll_row*(EDITOR_H-th))/maxsc;
        tk_draw_fill(w,sb_x,ty,4,th,COL_ACCENT);
    }
}

static void on_key(struct tk_window *w,struct tk_event *ev){
    uint8_t k=ev->key;
    if(k==0x13)save_file();
    else if(k==0x11){ tk_quit(w); return; }
    else if(k==0x0E)new_file();
    else if(k==KEY_UP){ if(g_cursor_row>0){ g_cursor_row--; int len=line_len(g_cursor_row); if(g_cursor_col>len)g_cursor_col=len; ensure_cursor_visible(); } }
    else if(k==KEY_DOWN){ if(g_cursor_row<g_line_count-1){ g_cursor_row++; int len=line_len(g_cursor_row); if(g_cursor_col>len)g_cursor_col=len; ensure_cursor_visible(); } }
    else if(k==KEY_LEFT){ if(g_cursor_col>0)g_cursor_col--; else if(g_cursor_row>0){ g_cursor_row--; g_cursor_col=line_len(g_cursor_row); ensure_cursor_visible(); } }
    else if(k==KEY_RIGHT){ int len=line_len(g_cursor_row); if(g_cursor_col<len)g_cursor_col++; else if(g_cursor_row<g_line_count-1){ g_cursor_row++; g_cursor_col=0; ensure_cursor_visible(); } }
    else if(k==KEY_HOME)g_cursor_col=0;
    else if(k==KEY_END)g_cursor_col=line_len(g_cursor_row);
    else if(k=='\b'||k==127)delete_back();
    else if(k=='\n'||k=='\r')split_line();
    else if(k=='\t'){ for(int ti=0;ti<4;ti++)insert_char(' '); }
    else if(k>=0x20&&k<=0x7E)insert_char((char)k);
    else return;
    tk_redraw(w);
}

int main(int argc,char **argv){
    my_memset(g_lines,0,sizeof(g_lines));
    g_line_count=1; g_cursor_row=g_cursor_col=g_scroll_row=0; g_modified=0; g_has_file=0; g_filename[0]='\0'; g_message[0]='\0';
    if(argc>1&&argv[1]&&argv[1][0]){ my_strcpy(g_filename,argv[1]); g_has_file=1; load_file(argv[1]); }

    if(tk_window_open(&win,WIN_W,WIN_H,"TobyEdit")!=0)return 1;
    tk_on_key(&win,on_key);
    struct tk_widget *root=tk_root(&win); tk_pad(root,0);
    { struct tk_widget *cv=tk_canvas(&win,root,paint); tk_grow(cv,1);
      tk_on_event(cv,on_event); }

    for(;;){
        if(tk_pump(&win))break;
        if(g_message[0]&&sys_clock_ms()>=g_msg_expire){ g_message[0]='\0'; tk_redraw(&win); }
        usleep(20000);
    }
    return 0;
}
