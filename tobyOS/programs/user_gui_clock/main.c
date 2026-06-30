/* user_gui_clock/main.c -- /bin/gui_clock
 *
 * Three-tab clock (analog/digital clock, stopwatch, timer). Migrated to the
 * TobyTK toolkit (toby/tk.h): the whole UI is a single full-window TK_CANVAS
 * that custom-draws the tab bar + active tab with the tk_draw_* primitives
 * (tk_draw_line for the analog face + hands), and a self-paced loop drives the
 * tick (tk_pump + a redraw throttle). The time source (SYS_CLOCK_MS) and all
 * stopwatch/timer state-machine logic are unchanged from the raw-gui_* version;
 * only the rendering + input layer moved to TobyTK.
 */

#include <toby/tk.h>
#include <unistd.h>

#define SYS_CLOCK_MS 48
static inline long sys_clock_ms(void){ long r; __asm__ volatile("syscall":"=a"(r):"0"((long)SYS_CLOCK_MS):"rcx","r11","memory"); return r; }

/* ---- fixed-point trig (60 positions) --------------------------------- */
static const int sin60[60] = {
       0,  105,  208,  309,  407,  500,  588,  669,  743,  809,
     866,  914,  951,  978,  995, 1000,  995,  978,  951,  914,
     866,  809,  743,  669,  588,  500,  407,  309,  208,  105,
       0, -105, -208, -309, -407, -500, -588, -669, -743, -809,
    -866, -914, -951, -978, -995,-1000, -995, -978, -951, -914,
    -866, -809, -743, -669, -588, -500, -407, -309, -208, -105 };
static const int cos60[60] = {
     1000,  995,  978,  951,  914,  866,  809,  743,  669,  588,
      500,  407,  309,  208,  105,    0, -105, -208, -309, -407,
     -500, -588, -669, -743, -809, -866, -914, -951, -978, -995,
    -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588,
     -500, -407, -309, -208, -105,    0,  105,  208,  309,  407,
      500,  588,  669,  743,  809,  866,  914,  951,  978,  995 };

#define WIN_W 320
#define WIN_H 400
#define TAB_H 30
#define TAB_COUNT 3

#define COL_BG       0x001A1A2Au
#define COL_PANEL    0x00252538u
#define COL_TAB_BG   0x00202035u
#define COL_ACCENT   0x004FC3F7u
#define COL_TEXT     0x00E0E0E0u
#define COL_DIM      0x00808890u
#define COL_WHITE    0x00FFFFFFu
#define COL_GREEN    0x0081C784u
#define COL_RED      0x00FF5252u
#define COL_ORANGE   0x00FFB74Du
#define COL_BTN      0x00303050u
#define COL_FLASH_BG 0x00402020u

#define CLOCK_CX 160
#define CLOCK_CY 175
#define CLOCK_R  100

enum { TAB_CLOCK, TAB_STOPWATCH, TAB_TIMER };
#define MAX_LAPS 5
enum { SW_STOPPED, SW_RUNNING, SW_PAUSED };
enum { TM_STOPPED, TM_RUNNING, TM_PAUSED, TM_DONE };

/* ---- state (global so the canvas callbacks can reach it) ------------- */
static struct tk_window win;
static int  active_tab = TAB_CLOCK;
static int  sw_state = SW_STOPPED;
static long sw_start_ms, sw_accum_ms, sw_laps[MAX_LAPS];
static int  sw_lap_count;
static int  tm_state = TM_STOPPED;
static long tm_duration, tm_remain, tm_start_ms, tm_start_val;

/* ---- helpers --------------------------------------------------------- */
static void fmt_2d(char *o,unsigned v){ o[0]=(char)('0'+(v/10)%10); o[1]=(char)('0'+v%10); }
static void fmt_uint(char *o,unsigned v){ char t[12]; int n=0; if(!v)t[n++]='0'; else while(v){t[n++]=(char)('0'+v%10);v/=10;} int i=0; while(n--)o[i++]=t[n]; o[i]='\0'; }
static int in_rect(int mx,int my,int rx,int ry,int rw,int rh){ return mx>=rx&&mx<rx+rw&&my>=ry&&my<ry+rh; }

#define TF(x,y,w,h,c) tk_draw_fill(&win,(x),(y),(w),(h),(c))
#define TL(a,b,c,d,e) tk_draw_line(&win,(a),(b),(c),(d),(e))
static void TT(int x,int y,const char *s,uint32_t fg){ tk_draw_text(&win,x,y,s,fg,15,0); }
static void CT(int cx,int y,const char *s,uint32_t fg){ int w=tk_text_width(s,15,0); tk_draw_text(&win,cx-w/2,y,s,fg,15,0); }

static void draw_thick_line(int x0,int y0,int x1,int y1,uint32_t c,int th){
    for(int dx=-th;dx<=th;dx++) for(int dy=-th;dy<=th;dy++) TL(x0+dx,y0+dy,x1+dx,y1+dy,c);
}

static void draw_btn(int x,int y,int w,int h,const char *label,uint32_t bg,uint32_t fg){
    TF(x,y,w,h,bg); TF(x,y,w,1,COL_ACCENT);
    int tw=tk_text_width(label,15,0);
    tk_draw_text(&win,x+(w-tw)/2,y+(h-15)/2,label,fg,15,0);
}

/* ---- tab bar --------------------------------------------------------- */
static void draw_tab_bar(int act){
    static const char *names[3]={"Clock","Stopwatch","Timer"};
    static const int widths[3]={107,106,107};
    int x=0;
    for(int i=0;i<TAB_COUNT;i++){
        uint32_t bg=(i==act)?COL_BG:COL_TAB_BG, fg=(i==act)?COL_ACCENT:COL_DIM;
        TF(x,0,widths[i],TAB_H,bg);
        int tw=tk_text_width(names[i],15,0);
        tk_draw_text(&win,x+(widths[i]-tw)/2,7,names[i],fg,15,0);
        if(i==act)TF(x,TAB_H-2,widths[i],2,COL_ACCENT);
        x+=widths[i];
    }
}
static int tab_from_click(int mx){ if(mx<107)return 0; if(mx<213)return 1; return 2; }

/* ---- clock tab ------------------------------------------------------- */
static void draw_clock_tab(long now_ms){
    unsigned total_s=(unsigned)(now_ms/1000), hrs=total_s/3600, mins=(total_s/60)%60, secs=total_s%60;
    for(int i=0;i<60;i++){
        int j=(i+1)%60;
        int x0=CLOCK_CX+sin60[i]*CLOCK_R/1000, y0=CLOCK_CY-cos60[i]*CLOCK_R/1000;
        int x1=CLOCK_CX+sin60[j]*CLOCK_R/1000, y1=CLOCK_CY-cos60[j]*CLOCK_R/1000;
        TL(x0,y0,x1,y1,COL_DIM);
    }
    for(int i=0;i<12;i++){
        int idx=i*5, r_out=CLOCK_R, r_in=(i%3==0)?CLOCK_R-15:CLOCK_R-10;
        uint32_t tc=(i%3==0)?COL_WHITE:COL_TEXT;
        int ox=CLOCK_CX+sin60[idx]*r_out/1000, oy=CLOCK_CY-cos60[idx]*r_out/1000;
        int ix=CLOCK_CX+sin60[idx]*r_in/1000,  iy=CLOCK_CY-cos60[idx]*r_in/1000;
        draw_thick_line(ox,oy,ix,iy,tc,0);
    }
    for(int i=0;i<60;i++){ if(i%5==0)continue;
        int px=CLOCK_CX+sin60[i]*(CLOCK_R-5)/1000, py=CLOCK_CY-cos60[i]*(CLOCK_R-5)/1000;
        TF(px,py,2,2,COL_DIM); }
    int h_pos=((hrs%12)*5+mins/12)%60;
    draw_thick_line(CLOCK_CX,CLOCK_CY,CLOCK_CX+sin60[h_pos]*55/1000,CLOCK_CY-cos60[h_pos]*55/1000,COL_WHITE,2);
    draw_thick_line(CLOCK_CX,CLOCK_CY,CLOCK_CX+sin60[mins]*78/1000,CLOCK_CY-cos60[mins]*78/1000,COL_TEXT,1);
    TL(CLOCK_CX,CLOCK_CY,CLOCK_CX+sin60[secs]*90/1000,CLOCK_CY-cos60[secs]*90/1000,COL_ACCENT);
    TF(CLOCK_CX-3,CLOCK_CY-3,6,6,COL_ACCENT);

    char ts[9]; fmt_2d(ts,hrs); ts[2]=':'; fmt_2d(ts+3,mins); ts[5]=':'; fmt_2d(ts+6,secs); ts[8]='\0';
    int panel_x=(WIN_W-112)/2;
    TF(panel_x,300,112,28,COL_PANEL); TF(panel_x,300,112,1,COL_ACCENT);
    CT(WIN_W/2,306,ts,COL_ACCENT);
    CT(WIN_W/2,340,"System Time",COL_DIM);
    TF(CLOCK_CX-2,360,5,5,(secs&1)?COL_GREEN:COL_DIM);
}

/* ---- stopwatch tab --------------------------------------------------- */
#define SW_BTN_Y 150
#define SW_BTN_H 32
#define SW_BTN1_X 30
#define SW_BTN1_W 120
#define SW_BTN2_X 170
#define SW_BTN2_W 120
static void draw_stopwatch_tab(int state,long elapsed_ms,long *laps,int lap_count){
    unsigned ms=(unsigned)elapsed_ms, tot=ms/1000, mm=tot/60, ss=tot%60, t=(ms/100)%10;
    CT(WIN_W/2,40,"Stopwatch",COL_ACCENT);
    char buf[9]; fmt_2d(buf,mm%100); buf[2]=':'; fmt_2d(buf+3,ss); buf[5]='.'; buf[6]=(char)('0'+t); buf[7]='\0';
    TF(30,62,WIN_W-60,44,COL_PANEL); TF(30,62,WIN_W-60,1,COL_ACCENT);
    CT(WIN_W/2,75,buf,COL_WHITE);
    const char *status="Ready"; uint32_t sc=COL_DIM;
    if(state==SW_RUNNING){status="Running";sc=COL_GREEN;} else if(state==SW_PAUSED){status="Paused";sc=COL_ORANGE;}
    CT(WIN_W/2,118,status,sc);
    if(state==SW_STOPPED){ draw_btn(SW_BTN1_X,SW_BTN_Y,SW_BTN1_W,SW_BTN_H,"Start",COL_GREEN,COL_BG); draw_btn(SW_BTN2_X,SW_BTN_Y,SW_BTN2_W,SW_BTN_H,"Reset",COL_BTN,COL_DIM); }
    else if(state==SW_RUNNING){ draw_btn(SW_BTN1_X,SW_BTN_Y,SW_BTN1_W,SW_BTN_H,"Stop",COL_RED,COL_WHITE); draw_btn(SW_BTN2_X,SW_BTN_Y,SW_BTN2_W,SW_BTN_H,"Lap",COL_BTN,COL_TEXT); }
    else { draw_btn(SW_BTN1_X,SW_BTN_Y,SW_BTN1_W,SW_BTN_H,"Resume",COL_GREEN,COL_BG); draw_btn(SW_BTN2_X,SW_BTN_Y,SW_BTN2_W,SW_BTN_H,"Reset",COL_BTN,COL_TEXT); }
    int lap_y=200; TF(30,lap_y-4,WIN_W-60,1,0x00303048u); TT(30,lap_y,"Laps",COL_ACCENT);
    if(lap_count==0)TT(50,lap_y+22,"No laps yet",COL_DIM);
    for(int i=0;i<lap_count&&i<MAX_LAPS;i++){
        unsigned lm=(unsigned)laps[i], ls=lm/1000;
        char lb[14]; lb[0]='#'; lb[1]=(char)('1'+i); lb[2]=' '; lb[3]=' ';
        fmt_2d(lb+4,(ls/60)%100); lb[6]=':'; fmt_2d(lb+7,ls%60); lb[9]='.'; lb[10]=(char)('0'+(lm/100)%10); lb[11]='\0';
        TT(50,lap_y+22+i*20,lb,COL_TEXT);
    }
}

/* ---- timer tab ------------------------------------------------------- */
#define TM_PRESET_Y 145
#define TM_PRESET_H 28
#define TM_BTN_Y 190
#define TM_BTN_H 32
static void draw_timer_tab(int state,long remain_ms,long duration_ms,long now_ms){
    long disp=(remain_ms>0)?remain_ms:0; unsigned tot=(unsigned)(disp/1000), mm=tot/60, ss=tot%60;
    char buf[6]; fmt_2d(buf,mm%100); buf[2]=':'; fmt_2d(buf+3,ss); buf[5]='\0';
    int flash=(state==TM_DONE)?(int)((now_ms/500)&1):0;
    uint32_t fg=COL_WHITE, panel_bg=COL_PANEL;
    if(state==TM_DONE){ fg=flash?COL_RED:COL_ORANGE; panel_bg=flash?COL_PANEL:COL_FLASH_BG; }
    CT(WIN_W/2,40,"Timer",COL_ACCENT);
    TF(30,62,WIN_W-60,44,panel_bg); TF(30,62,WIN_W-60,1,COL_ACCENT);
    CT(WIN_W/2,75,buf,fg);
    if(state==TM_DONE)CT(WIN_W/2,118,"TIME'S UP!",flash?COL_ORANGE:COL_RED);
    if(state==TM_STOPPED){ unsigned dur_m=(unsigned)(duration_ms/60000); char db[12]; db[0]='S';db[1]='e';db[2]='t';db[3]=':';db[4]=' '; fmt_uint(db+5,dur_m); int p=5; while(db[p])p++; db[p++]='m'; db[p]='\0'; CT(WIN_W/2,118,db,COL_DIM); }
    if(state==TM_RUNNING)CT(WIN_W/2,118,"Running",COL_GREEN);
    else if(state==TM_PAUSED)CT(WIN_W/2,118,"Paused",COL_ORANGE);
    if(state==TM_STOPPED||state==TM_DONE){ draw_btn(20,TM_PRESET_Y,80,TM_PRESET_H,"+1m",COL_BTN,COL_TEXT); draw_btn(120,TM_PRESET_Y,80,TM_PRESET_H,"+5m",COL_BTN,COL_TEXT); draw_btn(220,TM_PRESET_Y,80,TM_PRESET_H,"+10m",COL_BTN,COL_TEXT); }
    if(state==TM_STOPPED||state==TM_DONE){ uint32_t sbg=(duration_ms>0)?COL_GREEN:COL_BTN, sfg=(duration_ms>0)?COL_BG:COL_DIM; draw_btn(30,TM_BTN_Y,120,TM_BTN_H,"Start",sbg,sfg); draw_btn(170,TM_BTN_Y,120,TM_BTN_H,"Reset",COL_BTN,COL_DIM); }
    else if(state==TM_RUNNING){ draw_btn(30,TM_BTN_Y,120,TM_BTN_H,"Stop",COL_RED,COL_WHITE); draw_btn(170,TM_BTN_Y,120,TM_BTN_H,"Reset",COL_BTN,COL_TEXT); }
    else { draw_btn(30,TM_BTN_Y,120,TM_BTN_H,"Resume",COL_GREEN,COL_BG); draw_btn(170,TM_BTN_Y,120,TM_BTN_H,"Reset",COL_BTN,COL_TEXT); }
    if(duration_ms>0&&state!=TM_STOPPED){ int bar_y=240,bar_w=WIN_W-60; TF(30,bar_y,bar_w,8,COL_PANEL); long fill=disp*bar_w/duration_ms; if(fill>bar_w)fill=bar_w; if(fill>0)TF(30,bar_y,(int)fill,8,(state==TM_DONE)?COL_RED:COL_ACCENT); }
}

/* ---- canvas paint + event ------------------------------------------- */
static void paint(struct tk_window *w,struct tk_widget *c){
    (void)w;(void)c;
    long now=sys_clock_ms();
    TF(0,0,WIN_W,WIN_H,COL_BG);
    draw_tab_bar(active_tab);
    if(active_tab==TAB_CLOCK)draw_clock_tab(now);
    else if(active_tab==TAB_STOPWATCH){ long el=sw_accum_ms; if(sw_state==SW_RUNNING)el+=now-sw_start_ms; draw_stopwatch_tab(sw_state,el,sw_laps,sw_lap_count); }
    else draw_timer_tab(tm_state,tm_remain,tm_duration,now);
}

static void on_event(struct tk_window *w,struct tk_widget *c,struct tk_event *ev){
    (void)w;(void)c;
    if(ev->type!=TK_EV_MOUSE_DOWN)return;
    int mx=ev->x,my=ev->y;
    if(my<TAB_H){ active_tab=tab_from_click(mx); tk_redraw(&win); return; }
    if(active_tab==TAB_STOPWATCH){
        if(in_rect(mx,my,SW_BTN1_X,SW_BTN_Y,SW_BTN1_W,SW_BTN_H)){
            if(sw_state==SW_STOPPED||sw_state==SW_PAUSED){ sw_start_ms=sys_clock_ms(); sw_state=SW_RUNNING; }
            else if(sw_state==SW_RUNNING){ sw_accum_ms+=sys_clock_ms()-sw_start_ms; sw_state=SW_PAUSED; }
        }
        if(in_rect(mx,my,SW_BTN2_X,SW_BTN_Y,SW_BTN2_W,SW_BTN_H)){
            if(sw_state==SW_RUNNING&&sw_lap_count<MAX_LAPS) sw_laps[sw_lap_count++]=sw_accum_ms+(sys_clock_ms()-sw_start_ms);
            else if(sw_state!=SW_RUNNING){ sw_state=SW_STOPPED; sw_accum_ms=0; sw_start_ms=0; sw_lap_count=0; for(int i=0;i<MAX_LAPS;i++)sw_laps[i]=0; }
        }
    } else if(active_tab==TAB_TIMER){
        if(tm_state==TM_STOPPED||tm_state==TM_DONE){
            if(in_rect(mx,my,20,TM_PRESET_Y,80,TM_PRESET_H)){ tm_duration+=60000; tm_remain=tm_duration; }
            if(in_rect(mx,my,120,TM_PRESET_Y,80,TM_PRESET_H)){ tm_duration+=300000; tm_remain=tm_duration; }
            if(in_rect(mx,my,220,TM_PRESET_Y,80,TM_PRESET_H)){ tm_duration+=600000; tm_remain=tm_duration; }
        }
        if(in_rect(mx,my,30,TM_BTN_Y,120,TM_BTN_H)){
            if((tm_state==TM_STOPPED||tm_state==TM_DONE)&&tm_duration>0){ if(tm_state==TM_DONE)tm_remain=tm_duration; tm_start_ms=sys_clock_ms(); tm_start_val=tm_remain; tm_state=TM_RUNNING; }
            else if(tm_state==TM_PAUSED){ tm_start_ms=sys_clock_ms(); tm_start_val=tm_remain; tm_state=TM_RUNNING; }
            else if(tm_state==TM_RUNNING){ tm_remain=tm_start_val-(sys_clock_ms()-tm_start_ms); if(tm_remain<0)tm_remain=0; tm_state=TM_PAUSED; }
        }
        if(in_rect(mx,my,170,TM_BTN_Y,120,TM_BTN_H)){ tm_state=TM_STOPPED; tm_duration=0; tm_remain=0; }
    }
    tk_redraw(&win);
}

static void on_key(struct tk_window *w,struct tk_event *ev){ if(ev->key=='q'||ev->key==27)tk_quit(w); }

int main(int argc,char **argv){
    (void)argc;(void)argv;
    if(tk_window_open(&win,WIN_W,WIN_H,"Clock")!=0)return 1;
    tk_on_key(&win,on_key);
    struct tk_widget *root=tk_root(&win); tk_pad(root,0);
    struct tk_widget *cv=tk_canvas(&win,root,paint); tk_grow(cv,1); tk_on_event(cv,on_event);

    long last_draw=0;
    for(;;){
        if(tk_pump(&win))break;
        long now=sys_clock_ms();
        if(tm_state==TM_RUNNING){ tm_remain=tm_start_val-(now-tm_start_ms); if(tm_remain<=0){ tm_remain=0; tm_state=TM_DONE; } }
        int interval=(active_tab==TAB_CLOCK)?500:100;
        if(tm_state==TM_DONE)interval=250;
        if(now-last_draw>=interval){ last_draw=now; tk_redraw(&win); }
        usleep(30000);
    }
    return 0;
}
