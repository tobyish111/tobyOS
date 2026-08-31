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
#include <tobyos_slog.h>
#include <toby/image.h>
#include <toby/fileassoc.h>
#include <stdlib.h>
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

/* 2026-08-24: this file draws a CHARACTER GRID at CELL_W=8 columns, and
 * tk_draw_text_mono() finally delivers one -- it used to route to the
 * proportional TrueType renderer, so the columns never lined up and the
 * opaque background was dropped. The fixed cell is 8x16, taller than this
 * file's row pitch, so go through tk_draw_text_cell() to keep the pitch:
 * the glyph is centred in CELL_H and clipped, and the background covers
 * exactly CELL_H rows instead of bleeding into the line below. */
#define TK_MONO(w,x,y,s,fg,bg) tk_draw_text_cell((w),(x),(y),(s),(fg),(bg),CELL_H)
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
#define MODE_IMAGE 2
static int g_mode=MODE_TEXT;

/* ---- image mode -------------------------------------------------------
 *
 * The decoder (libtoby/src/image.c, stb_image) has been in the tree the
 * whole time with no standalone user -- only the browser called it. So the
 * file-type table could route a .png somewhere, but the only thing waiting
 * was a hex dump. It now decodes here.
 *
 * We decode from the PATH, not from g_buf: VIEWER_CAP is 32 KiB and almost
 * no real image fits, so decoding the text buffer would fail on nearly every
 * file that matters and look like a broken decoder.
 *
 * Only files the registry calls FTG_IMAGE are attempted. Trying every file
 * would let stb_image find a plausible header in an archive now and then and
 * present garbage as a picture. */
static toby_image_t *g_img;         /* decoded, or 0 */
static const char   *g_img_err;     /* why not, when it was supposed to be */
static uint32_t     *g_scaled;      /* nearest-neighbour scratch */
static int           g_scaled_w, g_scaled_h;
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

/* Bounded append: g_path can be as long as this whole buffer. */
static int log_cat(char *b,int p,int cap,const char *s){
    while(*s&&p<cap-1)b[p++]=*s++;
    b[p]='\0'; return p;
}

/* ---- TobyTK canvas --------------------------------------------------- */
static struct tk_window win;

static void draw_toolbar(struct tk_window *w){
    tk_draw_fill(w,0,0,WIN_W,TOOLBAR_H,COL_TOOLBAR);
    TK_MONO(w,8,9,"Text",(g_mode==MODE_TEXT)?COL_ACCENT:COL_DIM,COL_TOOLBAR);
    TK_MONO(w,44,9,"|",COL_DIM,COL_TOOLBAR);
    TK_MONO(w,56,9,"Hex",(g_mode==MODE_HEX)?COL_ACCENT:COL_DIM,COL_TOOLBAR);
    TK_MONO(w,88,9,"|",COL_DIM,COL_TOOLBAR);
    /* Dim when there is no image: the tab still SHOWS, so a picture that
     * failed to decode is visibly a picture we could not read, not a file
     * with no image in it. */
    TK_MONO(w,100,9,"Image",g_img?((g_mode==MODE_IMAGE)?COL_ACCENT:COL_DIM)
                                 :COL_DIM,COL_TOOLBAR);
    TK_MONO(w,152,9,"Search",COL_TEXT,COL_TOOLBAR);
    int plen=(int)my_strlen(g_path); int maxp=(WIN_W-160)/CELL_W;
    const char *disp=g_path; if(plen>maxp)disp=g_path+(plen-maxp);
    TK_MONO(w,WIN_W-(int)my_strlen(disp)*CELL_W-8,9,disp,COL_DIM,COL_TOOLBAR);
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
    TK_MONO(w,8,sy+5,status,COL_TEXT,COL_STATUS);
}
static void draw_text_mode(struct tk_window *w){
    int y=CONTENT_Y; char lnum[8];
    for(int r=0;r<ROWS;r++){
        int li=g_top_line+r; if(li>=g_line_count)break;
        uint32_t line_bg=(li==g_found_line)?COL_FOUND:COL_BG;
        tk_draw_fill(w,0,y,GUTTER_W,CELL_H,COL_GUTTER);
        uint_to_dec(lnum,li+1); int nlen=(int)my_strlen(lnum); int gx=GUTTER_W-(nlen+1)*CELL_W; if(gx<2)gx=2;
        TK_MONO(w,gx,y+1,lnum,COL_DIM,COL_GUTTER);
        if(li==g_found_line)tk_draw_fill(w,GUTTER_W,y,WIN_W-GUTTER_W,CELL_H,COL_FOUND);
        long start=g_line_off[li], end=(li+1<g_line_count)?g_line_off[li+1]:g_size;
        if(end>start&&g_buf[end-1]=='\n')end--;
        long len=end-start; if(len>TEXT_COLS)len=TEXT_COLS;
        char line[TEXT_COLS+1];
        for(long k=0;k<len;k++){ char c=g_buf[start+k]; if(c<0x20||c>0x7E)c='.'; line[k]=c; }
        line[len]='\0';
        TK_MONO(w,GUTTER_W+PAD,y+1,line,COL_TEXT,line_bg);
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
        TK_MONO(w,PAD,y+1,line,COL_TEXT,COL_BG);
        y+=CELL_H;
    }
}
static void draw_search_bar(struct tk_window *w){
    if(!g_search_active)return;
    int by=WIN_H-STATUS_H-24; tk_draw_fill(w,0,by,WIN_W,24,COL_TOOLBAR);
    char bar[SEARCH_MAX+12]; bar[0]='/'; bar[1]=' '; int p=2;
    for(int i=0;i<g_search_len&&p<(int)sizeof(bar)-2;i++)bar[p++]=g_search[i];
    bar[p++]='_'; bar[p]='\0';
    TK_MONO(w,8,by+6,bar,COL_ACCENT,COL_TOOLBAR);
}
/* Fit-to-window, aspect preserved, and NEVER upscaled past 1:1 -- blowing a
 * 16x16 icon up to fill 700px shows interpolation artefacts, not the icon.
 * Nearest-neighbour into a scratch buffer, then one blit: tk_draw_blit takes
 * a source pitch but does no scaling, and a per-pixel fill would be far too
 * slow at these sizes. */
static void draw_image_mode(struct tk_window *w){
    int ax=PAD, ay=CONTENT_Y;
    int aw=WIN_W-2*PAD, ah=WIN_H-CONTENT_Y-STATUS_H-PAD;
    if(aw<1||ah<1||!g_img||g_img->width<1||g_img->height<1)return;

    int dw=g_img->width, dh=g_img->height;
    if(dw>aw){ dh=(int)(((long)dh*aw)/dw); dw=aw; if(dh<1)dh=1; }
    if(dh>ah){ dw=(int)(((long)dw*ah)/dh); dh=ah; if(dw<1)dw=1; }

    if(dw!=g_scaled_w||dh!=g_scaled_h||!g_scaled){
        free(g_scaled);
        g_scaled=(uint32_t *)malloc((unsigned long)dw*(unsigned long)dh*4u);
        if(!g_scaled){ g_scaled_w=g_scaled_h=0; return; }
        g_scaled_w=dw; g_scaled_h=dh;
        for(int y=0;y<dh;y++){
            int sy=(int)(((long)y*g_img->height)/dh);
            const uint32_t *srow=g_img->pixels+(long)sy*g_img->width;
            uint32_t *drow=g_scaled+(long)y*dw;
            for(int x=0;x<dw;x++)
                drow[x]=srow[((long)x*g_img->width)/dw];
        }
    }
    tk_draw_blit(w,ax+(aw-dw)/2,ay+(ah-dh)/2,dw,dh,g_scaled,dw);
}

static void paint(struct tk_window *w,struct tk_widget *c){
    (void)c;
    tk_draw_fill(w,0,0,WIN_W,WIN_H,COL_BG);
    draw_toolbar(w);
    if(g_mode==MODE_IMAGE&&g_img)draw_image_mode(w);
    else if(g_mode==MODE_TEXT)draw_text_mode(w);
    else draw_hex_mode(w);
    draw_search_bar(w);
    draw_status(w);
}

/* Must track draw_toolbar() above: Text 8, Hex 56, Image 100, Search 152. */
static int toolbar_hit(int x,int y){ if(y<0||y>=TOOLBAR_H)return 0;
    if(x>=4&&x<44)return 1; if(x>=52&&x<84)return 2;
    if(x>=96&&x<148)return 4; if(x>=150&&x<206)return 3; return 0; }

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
    else if(hit==4&&g_img&&g_mode!=MODE_IMAGE){ g_mode=MODE_IMAGE; tk_redraw(w); }
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

    /* Ask the shared registry what this file IS, and only then try to decode
     * it. A file the table calls an image but stb_image refuses keeps the
     * reason in g_img_err, and the viewer opens in hex with that reason on
     * the status line -- a picture we cannot read should say so, not look
     * like a viewer that ignored the argument. */
    if(toby_file_type_group(g_path)==FTG_IMAGE){
        g_img=toby_image_load_file(g_path);
        /* Logged either way. The decode happens before any window exists, so
         * without this the only evidence of what it did is pixels on a
         * screen -- which no gate can read. */
        char m[192]; int q=0; m[0]=0; char d[12];
        if(g_img){
            g_mode=MODE_IMAGE;
            q=log_cat(m,q,(int)sizeof m,"decoded image ");
            uint_to_dec(d,g_img->width);  q=log_cat(m,q,(int)sizeof m,d);
            q=log_cat(m,q,(int)sizeof m,"x");
            uint_to_dec(d,g_img->height); q=log_cat(m,q,(int)sizeof m,d);
            q=log_cat(m,q,(int)sizeof m," path=");
            (void)log_cat(m,q,(int)sizeof m,g_path);
            tobylog_write(ABI_SLOG_LEVEL_INFO,"viewer",m);
        } else {
            g_img_err=toby_image_error(); if(!g_img_err)g_img_err="cannot decode";
            g_mode=MODE_HEX;
            q=log_cat(m,q,(int)sizeof m,"image decode FAILED (");
            q=log_cat(m,q,(int)sizeof m,g_img_err);
            q=log_cat(m,q,(int)sizeof m,") path=");
            (void)log_cat(m,q,(int)sizeof m,g_path);
            tobylog_write(ABI_SLOG_LEVEL_WARN,"viewer",m);
        }
    }

    if(tk_window_open(&win,WIN_W,WIN_H,"Viewer")!=0)return 1;
    tk_on_key(&win,on_key);
    struct tk_widget *root=tk_root(&win); tk_pad(root,0);
    struct tk_widget *cv=tk_canvas(&win,root,paint); tk_grow(cv,1); tk_on_event(cv,on_event);
    return tk_run(&win);
}
