/* mediaplayer/main.c -- Media Player for tobyOS.
 *
 * Migrated to the TobyTK toolkit (toby/tk.h). The player model is unchanged: a
 * playlist built from a directory's audio files, play/stop/next/prev with
 * repeat + shuffle, a seek bar, and volume. The UI moved to TobyTK -- the whole
 * window is a TK_CANVAS that custom-draws the playlist panel + album-art area +
 * transport controls; mouse hits come through the canvas on_event hook, keys
 * through the window key hook, and a self-paced loop ticks the seek bar /
 * auto-advance.
 */

#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <tobyos/abi/abi.h>
#include <toby/tk.h>

static long sc0(long n){long r;__asm__ volatile("syscall":"=a"(r):"0"(n):"rcx","r11","memory");return r;}
static long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static long sys_readdir(const char*p,void*b,int c){ return sc3(ABI_SYS_FS_READDIR,(long)p,(long)b,c); }
static long sys_clock_ms(void){ return sc0(ABI_SYS_CLOCK_MS); }

#define W 800
#define H 500
#define PLAYLIST_W 250
#define CONTROLS_H 80
#define NAME_MAX_D 64

#define C_BG       0x001e1e2eu
#define C_PLAYLIST 0x00252540u
#define C_PL_SEL   0x003a3a60u
#define C_PL_TEXT  0x00ccccccu
#define C_PLAY_BG  0x0016162au
#define C_CTRL_BG  0x00202038u
#define C_BTN      0x004444aau
#define C_BTN_ACT  0x006666ccu
#define C_TEXT     0x00ffffffu
#define C_DIM      0x00888899u
#define C_SEEK_BG  0x00333355u
#define C_SEEK_FG  0x006666ffu
#define C_VOL_BG   0x00333355u
#define C_VOL_FG   0x0044cc44u

#define MAX_PLAYLIST 64
struct playlist_entry { char name[NAME_MAX_D]; char path[256]; };
static struct playlist_entry playlist[MAX_PLAYLIST];
static int playlist_count, current_track=-1, playing, repeat_mode, shuffle, volume=80, playlist_scroll;
static long play_start_ms, track_duration_ms;

static void format_time(long ms,char *buf){ int s=(int)(ms/1000),m=s/60; s%=60; buf[0]='0'+m/10;buf[1]='0'+m%10;buf[2]=':';buf[3]='0'+s/10;buf[4]='0'+s%10;buf[5]='\0'; }

static void load_directory(const char *path){
    struct { char name[NAME_MAX_D]; unsigned type,size; } entries[128];
    int count=(int)sys_readdir(path,entries,128); playlist_count=0;
    for(int i=0;i<count&&playlist_count<MAX_PLAYLIST;i++){
        if(entries[i].type!=1)continue;
        int nl=(int)strlen(entries[i].name); int is_audio=0;
        if(nl>4){ const char *ext=entries[i].name+nl-4;
            if(!strcmp(ext,".mp3")||!strcmp(ext,".wav")||!strcmp(ext,".ogg")||!strcmp(ext,".m4a"))is_audio=1; }
        if(!is_audio)continue;
        memcpy(playlist[playlist_count].name,entries[i].name,nl+1);
        int pl=(int)strlen(path); memcpy(playlist[playlist_count].path,path,pl);
        if(pl>0&&path[pl-1]!='/')playlist[playlist_count].path[pl++]='/';
        memcpy(playlist[playlist_count].path+pl,entries[i].name,nl+1);
        playlist_count++;
    }
}
static void play_track(int idx){ if(idx<0||idx>=playlist_count)return; current_track=idx; playing=1; play_start_ms=sys_clock_ms(); track_duration_ms=180000; }
static void stop_playback(void){ playing=0; }
static void next_track(void){ if(!playlist_count)return; int n=current_track+1; if(n>=playlist_count){ if(repeat_mode==2)n=0; else { stop_playback(); return; } } play_track(n); }
static void prev_track(void){ if(!playlist_count)return; int p=current_track-1; if(p<0)p=playlist_count-1; play_track(p); }

/* ---- TobyTK canvas --------------------------------------------------- */
static struct tk_window win;

static void TT(struct tk_window *w,int x,int y,const char *s,uint32_t fg){ tk_draw_text(w,x,y,s,fg,14,0); }

static void paint(struct tk_window *w,struct tk_widget *c){
    (void)c;
    tk_draw_fill(w,0,0,W,H,C_BG);
    /* playlist panel */
    tk_draw_fill(w,0,0,PLAYLIST_W,H,C_PLAYLIST);
    tk_draw_text(w,8,8,"Playlist",C_TEXT,15,1);
    tk_draw_fill(w,0,26,PLAYLIST_W,1,C_DIM);
    int vis=(H-CONTROLS_H-30)/20;
    for(int i=0;i<vis&&(i+playlist_scroll)<playlist_count;i++){
        int idx=i+playlist_scroll, py=30+i*20;
        if(idx==current_track)tk_draw_fill(w,0,py,PLAYLIST_W,19,C_PL_SEL);
        char label[NAME_MAX_D+8]; snprintf(label,sizeof(label),"%d. %s",idx+1,playlist[idx].name);
        TT(w,8,py+3,label,C_PL_TEXT);
    }
    if(playlist_count==0)TT(w,8,40,"(no audio files)",C_DIM);

    /* playback area */
    int play_x=PLAYLIST_W, play_w=W-PLAYLIST_W, play_h=H-CONTROLS_H;
    tk_draw_fill(w,play_x,0,play_w,play_h,C_PLAY_BG);
    int art_sz=180, art_x=play_x+(play_w-art_sz)/2, art_y=40;
    tk_draw_fill(w,art_x,art_y,art_sz,art_sz,0x00333366u);
    tk_draw_text(w,art_x+art_sz/2-24,art_y+art_sz/2-8,"Music",C_DIM,16,0);
    if(current_track>=0&&current_track<playlist_count)
        tk_draw_text(w,play_x+40,art_y+art_sz+20,playlist[current_track].name,C_TEXT,20,1);

    /* controls */
    int cy=H-CONTROLS_H; tk_draw_fill(w,play_x,cy,play_w,CONTROLS_H,C_CTRL_BG);
    int seek_y=cy+8; tk_draw_fill(w,play_x+20,seek_y,play_w-40,6,C_SEEK_BG);
    if(playing&&track_duration_ms>0){
        long elapsed=sys_clock_ms()-play_start_ms; if(elapsed>track_duration_ms)elapsed=track_duration_ms;
        int fill_w=(int)((elapsed*(play_w-40))/track_duration_ms);
        tk_draw_fill(w,play_x+20,seek_y,fill_w,6,C_SEEK_FG);
        char pos[8],dur[8]; format_time(elapsed,pos); format_time(track_duration_ms,dur);
        TT(w,play_x+20,seek_y+10,pos,C_DIM); TT(w,play_x+play_w-68,seek_y+10,dur,C_DIM);
    }
    int btn_y=cy+35, btn_x=play_x+play_w/2-120;
    tk_draw_fill(w,btn_x,btn_y,50,30,C_BTN); TT(w,btn_x+10,btn_y+8,"|<<",C_TEXT); btn_x+=60;
    tk_draw_fill(w,btn_x,btn_y,60,30,playing?C_BTN_ACT:C_BTN); TT(w,btn_x+8,btn_y+8,playing?"Pause":"Play",C_TEXT); btn_x+=70;
    tk_draw_fill(w,btn_x,btn_y,50,30,C_BTN); TT(w,btn_x+8,btn_y+8,"Stop",C_TEXT); btn_x+=60;
    tk_draw_fill(w,btn_x,btn_y,50,30,C_BTN); TT(w,btn_x+10,btn_y+8,">>|",C_TEXT);
    int vol_x=play_x+play_w-120;
    TT(w,vol_x,btn_y+8,"Vol:",C_DIM); tk_draw_fill(w,vol_x+36,btn_y+12,70,6,C_VOL_BG);
    tk_draw_fill(w,vol_x+36,btn_y+12,volume*70/100,6,C_VOL_FG);
    TT(w,play_x+20,btn_y+8,repeat_mode==0?"Rep:Off":repeat_mode==1?"Rep:One":"Rep:All",repeat_mode?C_BTN_ACT:C_DIM);
    TT(w,play_x+100,btn_y+8,shuffle?"Shuf:On":"Shuf:Off",shuffle?C_BTN_ACT:C_DIM);
}

static void on_event(struct tk_window *w,struct tk_widget *c,struct tk_event *ev){
    (void)c;
    if(ev->type!=TK_EV_MOUSE_DOWN)return;
    int mx=ev->x,my=ev->y;
    if(mx<PLAYLIST_W&&my>=30){ int idx=(my-30)/20+playlist_scroll; if(idx>=0&&idx<playlist_count)play_track(idx); }
    int cy=H-CONTROLS_H, btn_y=cy+35;
    if(my>=btn_y&&my<btn_y+30){
        int play_x=PLAYLIST_W, play_w=W-PLAYLIST_W, btn_x=play_x+play_w/2-120;
        if(mx>=btn_x&&mx<btn_x+50)prev_track(); btn_x+=60;
        if(mx>=btn_x&&mx<btn_x+60){ if(playing)stop_playback(); else if(current_track>=0)play_track(current_track); else if(playlist_count>0)play_track(0); } btn_x+=70;
        if(mx>=btn_x&&mx<btn_x+50)stop_playback(); btn_x+=60;
        if(mx>=btn_x&&mx<btn_x+50)next_track();
    }
    tk_redraw(w);
}

static void on_key(struct tk_window *w,struct tk_event *ev){
    switch(ev->key){
    case ' ': if(playing)stop_playback(); else if(current_track>=0)play_track(current_track); else if(playlist_count>0)play_track(0); break;
    case 's': stop_playback(); break;
    case 'n': next_track(); break;
    case 'p': prev_track(); break;
    case 'r': repeat_mode=(repeat_mode+1)%3; break;
    case 'h': shuffle=!shuffle; break;
    case '+': if(volume<100)volume+=5; break;
    case '-': if(volume>0)volume-=5; break;
    case 'q': case 27: tk_quit(w); return;
    default: return;
    }
    tk_redraw(w);
}

int main(int argc,char **argv){
    if(argc>1)load_directory(argv[1]);
    if(playlist_count==0)load_directory("/");

    if(tk_window_open(&win,W,H,"Media Player")!=0)return 1;
    tk_on_key(&win,on_key);
    struct tk_widget *root=tk_root(&win); tk_pad(root,0);
    struct tk_widget *cv=tk_canvas(&win,root,paint); tk_grow(cv,1); tk_on_event(cv,on_event);

    long last_draw=sys_clock_ms();
    for(;;){
        if(tk_pump(&win))break;
        if(playing&&track_duration_ms>0){ long el=sys_clock_ms()-play_start_ms; if(el>=track_duration_ms){ if(repeat_mode==1)play_track(current_track); else next_track(); } }
        long now=sys_clock_ms();
        if(now-last_draw>500){ tk_redraw(&win); last_draw=now; }
        usleep(40000);
    }
    return 0;
}
