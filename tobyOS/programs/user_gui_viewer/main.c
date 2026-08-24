/* user_gui_viewer/main.c -- /bin/gui_viewer
 *
 * File viewer with text + hex view modes, search, line numbers, toolbar, and
 * status bar. Migrated to the TobyTK toolkit (toby/tk.h): the content is a
 * full-window TK_CANVAS drawn with the monospace tk_draw_text_mono (text/hex
 * grids need column alignment), the Text/Hex/Search toolbar items are hit-
 * tested from the canvas on_event hook, and keys flow through the window key
 * hook. All file indexing + search logic is unchanged.
 */

#include <toby/tk.h>
#include <unistd.h>

#define SYS_FS_READFILE 19
static inline long sys_fs_readfile(const char *path,void *out,unsigned long cap){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_FS_READFILE),"D"(path),"S"(out),"d"(cap):"rcx","r11","memory"); return r; }

/* ---- utility --------------------------------------------------------- */
static unsigned long my_strlen(const char *s){ const char *p=s; while(*p)p++; return (unsigned long)(p-s); }
static int my_memcmp(const void *a,const void *b,unsigned long n){ const uint8_t *pa=a,*pb=b; for(unsigned long i=0;i<n;i++)if(pa[i]!=pb[i])return pa[i]-pb[i]; return 0; }
static void byte_to_hex(char *o,uint8_t b){ const char *h="0123456789ABCDEF"; o[0]=h[b>>4]; o[1]=h[b&0xF]; }
static void uint_to_dec(char *buf,int val){ if(val<0)val=0; char t[12]; int i=0; if(!val)t[i++]='0'; else while(val>0){t[i++]='0'+(val%10);val/=10;} for(int j=0;j<i;j++)buf[j]=t[i-1-j]; buf[i]='\0'; }
static void offset_to_hex8(char *o,long off){ const char *h="0123456789ABCDEF"; for(int i=7;i>=0;i--){ o[i]=h[off&0xF]; off>>=4; } o[8]='\0'; }

/* ---- layout ---------------------------------------------------------- */
#define WIN_W 720
#define WIN_H 500
#define TOOLBAR_H 30
#define STATUS_H 22
#define GUTTER_W 40
#define PAD 6
#define CELL_W 8
#define CELL_H 14
#define CONTENT_Y TOOLBAR_H
#define CONTENT_H (WIN_H-TOOLBAR_H-STATUS_H)
#define ROWS (CONTENT_H/CELL_H)
#define TEXT_COLS ((WIN_W-GUTTER_W-PAD)/CELL_W)

#define COL_BG      0x001E1E2Eu
#define COL_TOOLBAR 0x002D2D3Du
#define COL_GUTTER  0x00252536u
#define COL_TEXT    0x00E0E0E0u
#define COL_DIM     0x00808890u
#define COL_ACCENT  0x004FC3F7u
#define COL_FOUND   0x00264F78u
#define COL_STATUS  0x002D2D3Du

/* ---- state (unchanged model) ----------------------------------------- */
#define VIEWER_CAP (32*1024)
static char g_buf[VIEWER_CAP+1];
static long g_size=0;
#define LINES_MAX 4096
static long g_line_off[LINES_MAX];
static int g_line_count=0, g_top_line=0;
#define MODE_TEXT 0
#define MODE_HEX 1
static int g_mode=MODE_TEXT;
#define SEARCH_MAX 64
static char g_search[SEARCH_MAX];
static int g_search_len=0, g_search_active=0, g_found_line=-1;
static int g_hex_total_lines=0;
static const char *g_path="/readme.txt";

static void index_lines(void){
    g_line_count=0; g_line_off[g_line_count++]=0;
    for(long i=0;i<g_size&&g_line_count<LINES_MAX;i++) if(g_buf[i]=='\n'&&i+1<g_size)g_line_off[g_line_count++]=i+1;
    g_hex_total_lines=(int)((g_size+15)/16);
}
static int max_top(void){ int total=(g_mode==MODE_HEX)?g_hex_total_lines:g_line_count; int mt=total-ROWS; return mt<0?0:mt; }
static void scroll_up(int n){ g_top_line-=n; if(g_top_line<0)g_top_line=0; }
static void scroll_down(int n){ g_top_line+=n; int mt=max_top(); if(g_top_line>mt)g_top_line=mt; }
static int search_forward(int start_line){
    if(g_search_len==0)return -1;
    for(int li=start_line;li<g_line_count;li++){
        long s=g_line_off[li], e=(li+1<g_line_count)?g_line_off[li+1]:g_size, len=e-s;
        for(long k=0;k+g_search_len<=len;k++) if(my_memcmp(&g_buf[s+k],g_search,(unsigned long)g_search_len)==0)return li;
    }
    return -1;
}

/* ---- TobyTK canvas --------------------------------------------------- */
static struct tk_window win;

static void draw_toolbar(struct tk_window *w){
    tk_draw_fill(w,0,0,WIN_W,TOOLBAR_H,COL_TOOLBAR);
    tk_draw_text_mono(w,8,9,"Text",(g_mode==MODE_TEXT)?COL_ACCENT:COL_DIM,COL_TOOLBAR);
    tk_draw_text_mono(w,44,9,"|",COL_DIM,COL_TOOLBAR);
    tk_draw_text_mono(w,56,9,"Hex",(g_mode==MODE_HEX)?COL_ACCENT:COL_DIM,COL_TOOLBAR);
    tk_draw_text_mono(w,96,9,"Search",COL_TEXT,COL_TOOLBAR);
    int plen=(int)my_strlen(g_path); int maxp=(WIN_W-160)/CELL_W;
    const char *disp=g_path; if(plen>maxp)disp=g_path+(plen-maxp);
    tk_draw_text_mono(w,WIN_W-(int)my_strlen(disp)*CELL_W-8,9,disp,COL_DIM,COL_TOOLBAR);
}
static void draw_status(struct tk_window *w){
    int sy=WIN_H-STATUS_H; tk_draw_fill(w,0,sy,WIN_W,STATUS_H,COL_STATUS);
    char status[128]; int pos=0;
    const char *fname=g_path; for(const char *p=g_path;*p;p++)if(*p=='/')fname=p+1;
    for(const char *p=fname;*p&&pos<40;)status[pos++]=*p++;
    status[pos++]=' '; status[pos++]=' ';
    char num[12];
    uint_to_dec(num,g_top_line+1); for(int i=0;num[i];i++)status[pos++]=num[i];
    status[pos++]='/';
    uint_to_dec(num,(g_mode==MODE_TEXT)?g_line_count:g_hex_total_lines); for(int i=0;num[i];i++)status[pos++]=num[i];
    status[pos++]=' '; status[pos++]=' ';
    char szbuf[12]; uint_to_dec(szbuf,(int)g_size); for(int i=0;szbuf[i];i++)status[pos++]=szbuf[i]; status[pos++]='B';
    status[pos++]=' '; status[pos++]=' ';
    if(g_mode==MODE_TEXT){ status[pos++]='T';status[pos++]='E';status[pos++]='X';status[pos++]='T'; }
    else { status[pos++]='H';status[pos++]='E';status[pos++]='X'; }
    status[pos]='\0';
    tk_draw_text_mono(w,8,sy+5,status,COL_TEXT,COL_STATUS);
}
static void draw_text_mode(struct tk_window *w){
    int y=CONTENT_Y; char lnum[8];
    for(int r=0;r<ROWS;r++){
        int li=g_top_line+r; if(li>=g_line_count)break;
        uint32_t line_bg=(li==g_found_line)?COL_FOUND:COL_BG;
        tk_draw_fill(w,0,y,GUTTER_W,CELL_H,COL_GUTTER);
        uint_to_dec(lnum,li+1); int nlen=(int)my_strlen(lnum); int gx=GUTTER_W-(nlen+1)*CELL_W; if(gx<2)gx=2;
        tk_draw_text_mono(w,gx,y+1,lnum,COL_DIM,COL_GUTTER);
        if(li==g_found_line)tk_draw_fill(w,GUTTER_W,y,WIN_W-GUTTER_W,CELL_H,COL_FOUND);
        long start=g_line_off[li], end=(li+1<g_line_count)?g_line_off[li+1]:g_size;
        if(end>start&&g_buf[end-1]=='\n')end--;
        long len=end-start; if(len>TEXT_COLS)len=TEXT_COLS;
        char line[TEXT_COLS+1];
        for(long k=0;k<len;k++){ char c=g_buf[start+k]; if(c<0x20||c>0x7E)c='.'; line[k]=c; }
        line[len]='\0';
        tk_draw_text_mono(w,GUTTER_W+PAD,y+1,line,COL_TEXT,line_bg);
        y+=CELL_H;
    }
}
static void draw_hex_mode(struct tk_window *w){
    int y=CONTENT_Y; char line[80];
    for(int r=0;r<ROWS;r++){
        int row_idx=g_top_line+r; long off=(long)row_idx*16; if(off>=g_size)break;
        offset_to_hex8(line,off); line[8]=' ';line[9]=' ';line[10]='|';line[11]=' '; int pos=12;
        int count=16; if(off+count>g_size)count=(int)(g_size-off);
        for(int i=0;i<16;i++){ if(i==8)line[pos++]=' ';
            if(i<count){ byte_to_hex(&line[pos],(uint8_t)g_buf[off+i]); pos+=2; } else { line[pos++]=' '; line[pos++]=' '; }
            line[pos++]=' '; }
        line[pos++]='|'; line[pos++]=' ';
        for(int i=0;i<16;i++){ if(i<count){ uint8_t c=(uint8_t)g_buf[off+i]; line[pos++]=(c>=0x20&&c<=0x7E)?(char)c:'.'; } else line[pos++]=' '; }
        line[pos]='\0';
        tk_draw_text_mono(w,PAD,y+1,line,COL_TEXT,COL_BG);
        y+=CELL_H;
    }
}
static void draw_search_bar(struct tk_window *w){
    if(!g_search_active)return;
    int by=WIN_H-STATUS_H-24; tk_draw_fill(w,0,by,WIN_W,24,COL_TOOLBAR);
    char bar[SEARCH_MAX+12]; bar[0]='/'; bar[1]=' '; int p=2;
    for(int i=0;i<g_search_len&&p<(int)sizeof(bar)-2;i++)bar[p++]=g_search[i];
    bar[p++]='_'; bar[p]='\0';
    tk_draw_text_mono(w,8,by+6,bar,COL_ACCENT,COL_TOOLBAR);
}
static void paint(struct tk_window *w,struct tk_widget *c){
    (void)c;
    tk_draw_fill(w,0,0,WIN_W,WIN_H,COL_BG);
    draw_toolbar(w);
    if(g_mode==MODE_TEXT)draw_text_mode(w); else draw_hex_mode(w);
    draw_search_bar(w);
    draw_status(w);
}

static int toolbar_hit(int x,int y){ if(y<0||y>=TOOLBAR_H)return 0; if(x>=4&&x<40)return 1; if(x>=52&&x<88)return 2; if(x>=92&&x<148)return 3; return 0; }

static void on_event(struct tk_window *w,struct tk_widget *c,struct tk_event *ev){
    (void)c;
    if(ev->type==TK_EV_WHEEL){
        if(!ev->wheel)return;
        if(ev->wheel>0)scroll_up((int)ev->wheel*TK_WHEEL_LINES);
        else           scroll_down((int)-ev->wheel*TK_WHEEL_LINES);
        tk_redraw(w);
        return;
    }
    if(ev->type!=TK_EV_MOUSE_DOWN)return;
    int hit=toolbar_hit(ev->x,ev->y);
    if(hit==1&&g_mode!=MODE_TEXT){ g_mode=MODE_TEXT; g_top_line=0; tk_redraw(w); }
    else if(hit==2&&g_mode!=MODE_HEX){ g_mode=MODE_HEX; g_top_line=0; tk_redraw(w); }
    else if(hit==3){ g_search_active=1; g_search_len=0; tk_redraw(w); }
}

static void on_key(struct tk_window *w,struct tk_event *ev){
    uint8_t k=ev->key;
    if(g_search_active){
        if(k==27)g_search_active=0;
        else if(k=='\n'||k=='\r'){ g_search[g_search_len]='\0'; int f=search_forward(g_top_line); if(f<0)f=search_forward(0); if(f>=0){ g_found_line=f; g_top_line=f; int mt=max_top(); if(g_top_line>mt)g_top_line=mt; } g_search_active=0; }
        else if(k=='\b'||k==127){ if(g_search_len>0)g_search_len--; }
        else if(k>=0x20&&k<=0x7E){ if(g_search_len<SEARCH_MAX-1)g_search[g_search_len++]=(char)k; }
        tk_redraw(w); return;
    }
    switch(k){
    case 'j': case '\n': scroll_down(1); break;
    case 'k': case '\b': scroll_up(1); break;
    case 'd': case ' ': scroll_down(ROWS); break;
    case 'u': scroll_up(ROWS); break;
    case 'g': g_top_line=0; break;
    case 'G': g_top_line=max_top(); break;
    case 'f': case '/': g_search_active=1; g_search_len=0; break;
    case 'n': if(g_search_len>0){ g_search[g_search_len]='\0'; int start=(g_found_line>=0)?g_found_line+1:g_top_line; int f=search_forward(start); if(f<0)f=search_forward(0); if(f>=0){ g_found_line=f; g_top_line=f; int mt=max_top(); if(g_top_line>mt)g_top_line=mt; } } break;
    case 'q': case 27: tk_quit(w); return;
    default: return;
    }
    tk_redraw(w);
}

int main(int argc,char **argv){
    g_path=(argc>=2)?argv[1]:"/readme.txt";
    long n=sys_fs_readfile(g_path,g_buf,VIEWER_CAP);
    if(n<0){ const char *msg="gui_viewer: cannot read file"; g_size=(long)my_strlen(msg); for(long i=0;i<g_size;i++)g_buf[i]=msg[i]; g_buf[g_size]='\0'; }
    else { g_size=n; g_buf[g_size]='\0'; }
    index_lines();

    if(tk_window_open(&win,WIN_W,WIN_H,"Viewer")!=0)return 1;
    tk_on_key(&win,on_key);
    struct tk_widget *root=tk_root(&win); tk_pad(root,0);
    struct tk_widget *cv=tk_canvas(&win,root,paint); tk_grow(cv,1); tk_on_event(cv,on_event);
    return tk_run(&win);
}
