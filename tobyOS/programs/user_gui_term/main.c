/* user_gui_term/main.c -- /bin/gui_term, the GUI terminal emulator.
 *
 * 2026-08-24: THIS WINDOW NOW RUNS REAL /bin/tsh ON A REAL PTY.
 * It previously called SYS_TERM_OPEN, which is not a shell at all -- it is
 * a kernel-side line reader (src/term.c) with its own small builtin set
 * that drops every control key. So the desktop's Terminal icon opened
 * something that merely resembled a shell, while the actual system shell
 * was only reachable over the serial console. See shell_start() below.
 *
 * TobyTK toolkit (toby/tk.h). The terminal core is unchanged:
 * an 80x30 cell grid with a 200-line scrollback ring, an ANSI/VT state machine
 * (ESC[...m SGR colours, cursor moves, erase/insert/delete). The UI is TobyTK: the whole grid is
 * a full-window TK_CANVAS drawn with tk_draw_text_mono (the 8x16 fixed-cell VGA
 * font -- proportional TTF would not column-align), keys flow through the
 * window key hook to the PTY, and a self-paced loop pumps PTY output + blinks
 * the cursor.
 */

#include <toby/tk.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>

#define SYS_CLOCK_MS   48
static inline uint32_t sys_clock_ms(void){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_CLOCK_MS):"rcx","r11","memory"); return (uint32_t)r; }

/* ---- the shell behind the window ------------------------------------
 *
 * This used to call SYS_TERM_OPEN, which is NOT a shell: it is a
 * kernel-side line reader in src/term.c that accumulates a command,
 * calls execute_line(), and DROPS every control key -- no arrows, no
 * history, no pipes, no job control, and a builtin set entirely separate
 * from tsh's. The terminal icon therefore never opened the system shell.
 *
 * It now spawns REAL /bin/tsh on a real pty, so the window gets the same
 * shell as the serial console and everything tsh can do works here.
 *
 * This is only possible because tcsetattr() now actually reaches the
 * kernel (libtoby/src/termios.c) -- before that fix a shell on a pty
 * could not have put the line discipline where it needs it. */
extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

#define TIOCSPTLCK  0x40045431UL
#define TIOCGPTN    0x80045430UL
#define TIOCSWINSZ  0x5414UL
#define TIOCSPGRP   0x5410UL
#define FIONREAD    0x541BUL
struct winsize_t { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

static int   g_mfd = -1;      /* pty master */
static pid_t g_shell_pid = -1;
static int   g_shell_gone = 0;

#define KEY_PGUP 0x86
#define KEY_PGDN 0x87

/* ---- grid geometry --------------------------------------------------- */
#define COLS 80
#define ROWS 30
#define SCROLLBACK 200
#define CELL_W 8
#define CELL_H 16
#define WIN_W (COLS*CELL_W)
#define WIN_H (ROWS*CELL_H)

typedef struct { uint8_t ch, fg, bg; } cell_t;

static const uint32_t g_palette[16] = {
    0x00000000,0x00CC0000,0x0000CC00,0x00CCCC00,0x000000CC,0x00CC00CC,0x0000CCCC,0x00CCCCCC,
    0x00333333,0x00FF3333,0x0033FF33,0x00FFFF33,0x003333FF,0x00FF33FF,0x0033FFFF,0x00FFFFFF };
#define COL_CURSOR 0x00FFD060u

/* ---- terminal state (unchanged) -------------------------------------- */
static cell_t g_ring[SCROLLBACK][COLS];
static int g_ring_start, g_ring_count, g_scroll_offset;
static int g_cx, g_cy, g_need_redraw;
static uint8_t g_cur_fg, g_cur_bg; static int g_bold;
static uint32_t g_last_blink; static int g_cursor_visible;

#define STATE_NORMAL 0
#define STATE_ESC 1
#define STATE_CSI 2
#define MAX_CSI_PARAMS 8
static int g_esc_state, g_csi_params[MAX_CSI_PARAMS], g_csi_nparams, g_csi_priv;

static cell_t *ring_line(int o){ return g_ring[(g_ring_start+o)%SCROLLBACK]; }
static cell_t *screen_line(int row){ return ring_line(g_ring_count-ROWS+row); }
static cell_t *view_line(int row){ return ring_line(g_ring_count-ROWS-g_scroll_offset+row); }
static uint8_t effective_fg(void){ uint8_t fg=g_cur_fg; if(g_bold&&fg<8)fg+=8; return fg; }

static void clear_line_default(cell_t *l){ for(int x=0;x<COLS;x++){ l[x].ch=' '; l[x].fg=7; l[x].bg=0; } }
static void init_ring(void){
    g_ring_start=0; g_ring_count=ROWS;
    for(int i=0;i<SCROLLBACK;i++)clear_line_default(g_ring[i]);
    g_cx=g_cy=g_scroll_offset=0; g_need_redraw=1; g_cur_fg=7; g_cur_bg=0; g_bold=0;
    g_esc_state=STATE_NORMAL; g_cursor_visible=1;
}
static void grid_clear(void){ for(int r=0;r<ROWS;r++)clear_line_default(screen_line(r)); g_cx=g_cy=0; g_need_redraw=1; }
static void scroll_up(void){ if(g_ring_count<SCROLLBACK)g_ring_count++; else g_ring_start=(g_ring_start+1)%SCROLLBACK; clear_line_default(screen_line(ROWS-1)); }
static void grid_newline(void){ g_cx=0; if(g_cy+1>=ROWS)scroll_up(); else g_cy++; }

static void csi_dispatch(char cmd){
    int p0=g_csi_nparams>0?g_csi_params[0]:0;
    int p1=g_csi_nparams>1?g_csi_params[1]:0;
    if(g_csi_priv)return;
    switch(cmd){
    case 'm':{
        if(g_csi_nparams==0||(g_csi_nparams==1&&p0==0)){ g_cur_fg=7; g_cur_bg=0; g_bold=0; break; }
        for(int i=0;i<g_csi_nparams;i++){ int p=g_csi_params[i];
            if(p==0){g_cur_fg=7;g_cur_bg=0;g_bold=0;} else if(p==1)g_bold=1; else if(p==22)g_bold=0;
            else if(p>=30&&p<=37)g_cur_fg=(uint8_t)(p-30); else if(p==39)g_cur_fg=7;
            else if(p>=40&&p<=47)g_cur_bg=(uint8_t)(p-40); else if(p==49)g_cur_bg=0; }
        break; }
    case 'A':{ int n=p0>0?p0:1; g_cy-=n; if(g_cy<0)g_cy=0; break; }
    case 'B':{ int n=p0>0?p0:1; g_cy+=n; if(g_cy>=ROWS)g_cy=ROWS-1; break; }
    case 'C':{ int n=p0>0?p0:1; g_cx+=n; if(g_cx>=COLS)g_cx=COLS-1; break; }
    case 'D':{ int n=p0>0?p0:1; g_cx-=n; if(g_cx<0)g_cx=0; break; }
    case 'G': g_cx=(p0>0?p0-1:0); if(g_cx>=COLS)g_cx=COLS-1; break;
    case 'd': g_cy=(p0>0?p0-1:0); if(g_cy>=ROWS)g_cy=ROWS-1; break;
    case 'H': case 'f': g_cy=(p0>0?p0-1:0); g_cx=(p1>0?p1-1:0); if(g_cy>=ROWS)g_cy=ROWS-1; if(g_cx>=COLS)g_cx=COLS-1; break;
    case 'J':{ cell_t *line;
        if(p0==0){ line=screen_line(g_cy); for(int x=g_cx;x<COLS;x++){line[x].ch=' ';line[x].fg=g_cur_fg;line[x].bg=g_cur_bg;}
            for(int r=g_cy+1;r<ROWS;r++){line=screen_line(r); for(int x=0;x<COLS;x++){line[x].ch=' ';line[x].fg=g_cur_fg;line[x].bg=g_cur_bg;}} }
        else if(p0==1){ for(int r=0;r<g_cy;r++){line=screen_line(r); for(int x=0;x<COLS;x++){line[x].ch=' ';line[x].fg=g_cur_fg;line[x].bg=g_cur_bg;}}
            line=screen_line(g_cy); for(int x=0;x<=g_cx&&x<COLS;x++){line[x].ch=' ';line[x].fg=g_cur_fg;line[x].bg=g_cur_bg;} }
        else if(p0==2)grid_clear();
        break; }
    case 'K':{ cell_t *line=screen_line(g_cy); int start,end;
        if(p0==1){start=0;end=g_cx+1;} else if(p0==2){start=0;end=COLS;} else {start=g_cx;end=COLS;}
        if(end>COLS)end=COLS; for(int x=start;x<end;x++){line[x].ch=' ';line[x].fg=g_cur_fg;line[x].bg=g_cur_bg;} break; }
    case '@':{ int n=p0>0?p0:1; cell_t *line=screen_line(g_cy);
        for(int x=COLS-1;x>=g_cx+n;x--)line[x]=line[x-n];
        for(int x=g_cx;x<g_cx+n&&x<COLS;x++){line[x].ch=' ';line[x].fg=g_cur_fg;line[x].bg=g_cur_bg;} break; }
    case 'P':{ int n=p0>0?p0:1; cell_t *line=screen_line(g_cy);
        for(int x=g_cx;x+n<COLS;x++)line[x]=line[x+n];
        int fs=COLS-n; if(fs<g_cx)fs=g_cx; for(int x=fs;x<COLS;x++){line[x].ch=' ';line[x].fg=g_cur_fg;line[x].bg=g_cur_bg;} break; }
    case 'L':{ int n=p0>0?p0:1; for(int y=ROWS-1;y>=g_cy+n;y--){ cell_t *d=screen_line(y),*s=screen_line(y-n); for(int x=0;x<COLS;x++)d[x]=s[x]; }
        for(int y=g_cy;y<g_cy+n&&y<ROWS;y++)clear_line_default(screen_line(y)); break; }
    case 'M':{ int n=p0>0?p0:1; for(int y=g_cy;y+n<ROWS;y++){ cell_t *d=screen_line(y),*s=screen_line(y+n); for(int x=0;x<COLS;x++)d[x]=s[x]; }
        int fs=ROWS-n; if(fs<g_cy)fs=g_cy; for(int y=fs;y<ROWS;y++)clear_line_default(screen_line(y)); break; }
    }
}

static void vt_putc(uint8_t c){
    switch(g_esc_state){
    case STATE_NORMAL:
        if(c=='\033')g_esc_state=STATE_ESC;
        else if(c=='\r')g_cx=0;
        else if(c=='\n')grid_newline();
        else if(c=='\b'){ if(g_cx>0)g_cx--; }
        else if(c=='\t'){ g_cx=(g_cx+8)&~7; if(g_cx>=COLS)g_cx=COLS-1; }
        else if(c==0x0C)grid_clear();
        else if(c>=0x20&&c<0x7F){ if(g_cx>=COLS)grid_newline(); cell_t *line=screen_line(g_cy); line[g_cx].ch=c; line[g_cx].fg=effective_fg(); line[g_cx].bg=g_cur_bg; g_cx++; }
        break;
    case STATE_ESC:
        if(c=='['){ g_esc_state=STATE_CSI; g_csi_nparams=0; g_csi_priv=0; for(int i=0;i<MAX_CSI_PARAMS;i++)g_csi_params[i]=0; }
        else g_esc_state=STATE_NORMAL;
        break;
    case STATE_CSI:
        if(c=='?')g_csi_priv=1;
        else if(c>='0'&&c<='9'){ if(g_csi_nparams==0)g_csi_nparams=1; int idx=g_csi_nparams-1; if(idx<MAX_CSI_PARAMS)g_csi_params[idx]=g_csi_params[idx]*10+(c-'0'); }
        else if(c==';'){ if(g_csi_nparams==0)g_csi_nparams=1; if(g_csi_nparams<MAX_CSI_PARAMS){ g_csi_params[g_csi_nparams]=0; g_csi_nparams++; } }
        else if(c>=0x40&&c<=0x7E){ if(g_csi_nparams==0)g_csi_nparams=1; csi_dispatch((char)c); g_esc_state=STATE_NORMAL; }
        else g_esc_state=STATE_NORMAL;
        break;
    }
    g_need_redraw=1;
}

/* ---- TobyTK canvas --------------------------------------------------- */
static struct tk_window win;

static void paint(struct tk_window *w,struct tk_widget *c){
    (void)c;
    tk_draw_fill(w,0,0,WIN_W,WIN_H,g_palette[0]);
    char run[COLS+1];
    for(int y=0;y<ROWS;y++){
        cell_t *line=view_line(y);
        int x=0;
        while(x<COLS){
            uint8_t fg=line[x].fg, bg=line[x].bg; int start=x;
            while(x<COLS&&line[x].fg==fg&&line[x].bg==bg){ uint8_t ch=line[x].ch; run[x-start]=ch?(char)ch:' '; x++; }
            run[x-start]='\0';
            tk_draw_text_mono(w,start*CELL_W,y*CELL_H,run,g_palette[fg<16?fg:7],g_palette[bg<16?bg:0]);
        }
    }
    if(g_cursor_visible&&g_scroll_offset==0&&g_cx<COLS){
        int px=g_cx*CELL_W, py=g_cy*CELL_H;
        tk_draw_fill(w,px,py,CELL_W,CELL_H,COL_CURSOR);
        cell_t *cl=screen_line(g_cy); char cb[2]; cb[0]=cl[g_cx].ch?(char)cl[g_cx].ch:' '; cb[1]='\0';
        tk_draw_text_mono(w,px,py,cb,g_palette[0],COL_CURSOR);
    }
}

/* Wheel over the terminal walks the scrollback, same axis as PgUp/PgDn.
 * g_scroll_offset counts lines BACK from the live tail, so a positive
 * detent (away from the user) increases it. */
static void on_event(struct tk_window *w,struct tk_widget *c,struct tk_event *ev){
    (void)c;
    if(ev->type!=TK_EV_WHEEL||!ev->wheel)return;
    g_scroll_offset+=(int)ev->wheel*TK_WHEEL_LINES;
    int mo=g_ring_count-ROWS; if(mo<0)mo=0;
    if(g_scroll_offset>mo)g_scroll_offset=mo;
    if(g_scroll_offset<0)g_scroll_offset=0;
    g_need_redraw=1; tk_redraw(w);
}

static void term_send(const char *s,int n){
    if(g_mfd>=0&&n>0) (void)write(g_mfd,s,(size_t)n);
}

static void on_key(struct tk_window *w,struct tk_event *ev){
    uint8_t k=ev->key; if(k==0)return;
    if(k==KEY_PGUP){ g_scroll_offset+=ROWS/2; int mo=g_ring_count-ROWS; if(mo<0)mo=0; if(g_scroll_offset>mo)g_scroll_offset=mo; g_need_redraw=1; tk_redraw(w); return; }
    if(k==KEY_PGDN){ g_scroll_offset-=ROWS/2; if(g_scroll_offset<0)g_scroll_offset=0; g_need_redraw=1; tk_redraw(w); return; }

    /* The GUI hands special keys over as single bytes 0x80.. ; a shell on
     * a terminal expects the ANSI sequences. Translating here is what
     * makes arrow keys, Home and End work in tsh (and in anything it
     * runs) instead of arriving as unprintable garbage. */
    switch(k){
    case TK_KEY_UP:    term_send("\033[A",3); return;
    case TK_KEY_DOWN:  term_send("\033[B",3); return;
    case TK_KEY_RIGHT: term_send("\033[C",3); return;
    case TK_KEY_LEFT:  term_send("\033[D",3); return;
    case TK_KEY_HOME:  term_send("\033[H",3); return;
    case TK_KEY_END:   term_send("\033[F",3); return;
    default: break;
    }
    if(k>=0x80) return;                 /* other specials: not ours to invent */
    char kb=(char)k; term_send(&kb,1);
}

/* Spawn /bin/tsh on a fresh pty. Returns 0 on success. */
static int shell_start(void){
    int mfd=open("/dev/ptmx",O_RDWR);
    if(mfd<0)return -1;
    int unlock=0; (void)ioctl(mfd,TIOCSPTLCK,&unlock);
    int idx=-1;
    if(ioctl(mfd,TIOCGPTN,&idx)!=0||idx<0){ close(mfd); return -2; }
    char spath[32]; snprintf(spath,sizeof spath,"/dev/pts/%d",idx);
    int sfd=open(spath,O_RDWR);
    if(sfd<0){ close(mfd); return -3; }

    /* Tell the pty how big the window is, so tsh and anything it runs
     * (tedit, tvi, top) lay out for 80x30 rather than the 80x24 default. */
    struct winsize_t ws={ (unsigned short)ROWS,(unsigned short)COLS,0,0 };
    (void)ioctl(sfd,TIOCSWINSZ,&ws);

    /* Make our process group the pty's foreground group BEFORE the shell
     * exists, so its job-control init sees itself foreground from its
     * first instruction and never stops. Same etiquette /bin/ttyparity
     * needs. */
    pid_t self_pg=getpgrp();
    (void)ioctl(sfd,TIOCSPGRP,&self_pg);

    char *const envp[]={ (char *)"PATH=/bin",(char *)"HOME=/",
                         (char *)"TERM=vt100",(char *)"LANG=C",0 };
    char *argv[]={ (char *)"tsh",0 };
    pid_t pid=toby_spawn("/bin/tsh",argv,envp,sfd,sfd,sfd);
    close(sfd);
    if(pid<0){ close(mfd); return -4; }
    g_mfd=mfd; g_shell_pid=pid;
    return 0;
}

/* Non-blocking drain of the shell's output.
 *
 * FIONREAD first, ALWAYS: a read() on the pty master BLOCKS when the
 * shell has said nothing, and this is the GUI thread -- a blocking read
 * here freezes the whole window, not just the terminal. */
static int shell_pump(void){
    if(g_mfd<0)return 0;
    char buf[512];
    int got=0,avail=0;
    while(ioctl(g_mfd,FIONREAD,&avail)==0&&avail>0){
        int want=avail>(int)sizeof buf?(int)sizeof buf:avail;
        long n=read(g_mfd,buf,(size_t)want);
        if(n<=0)break;
        if(g_scroll_offset>0)g_scroll_offset=0;
        for(long i=0;i<n;i++)vt_putc((uint8_t)buf[i]);
        got=1;
        if(avail<=want)break;
    }
    return got;
}

int main(int argc,char **argv){
    (void)argc;(void)argv;
    if(tk_window_open(&win,WIN_W,WIN_H,"Terminal")!=0)return 1;
    if(shell_start()!=0){
        /* Say WHY rather than vanishing: a terminal window that closes
         * instantly is indistinguishable from one that never launched. */
        const char *m="tobyOS terminal: cannot start /bin/tsh on a pty\r\n";
        for(const char *p=m;*p;p++)vt_putc((uint8_t)*p);
        g_need_redraw=1;
    }
    tk_on_key(&win,on_key);
    struct tk_widget *root=tk_root(&win); tk_pad(root,0);
    struct tk_widget *cv=tk_canvas(&win,root,paint); tk_grow(cv,1);
    tk_on_event(cv,on_event);

    init_ring();
    g_last_blink=sys_clock_ms();

    for(;;){
        if(tk_pump(&win))break;
        if(shell_pump())g_need_redraw=1;

        /* Reap the shell if it exited, and say so in the window rather
         * than leaving a dead terminal that silently ignores typing. */
        if(g_shell_pid>0&&!g_shell_gone){
            int st=0;
            if(waitpid(g_shell_pid,&st,WNOHANG)==g_shell_pid){
                g_shell_gone=1;
                const char *m="\r\n[tsh exited -- close this window]\r\n";
                for(const char *p=m;*p;p++)vt_putc((uint8_t)*p);
                g_need_redraw=1;
            }
        }
        uint32_t now=sys_clock_ms();
        if(now-g_last_blink>=500){ g_cursor_visible=!g_cursor_visible; g_last_blink=now; g_need_redraw=1; }
        if(g_need_redraw){ tk_redraw(&win); g_need_redraw=0; }
        usleep(20000);
    }
    return 0;
}
