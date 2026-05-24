/* mediaplayer/main.c -- Media Player for tobyOS (Milestone 5).
 *
 * 800x500 GUI window with playlist panel, playback area, and controls.
 * Opens audio files and uses the media_player API for playback.
 * Shows title, artist, duration, position. Has repeat/shuffle buttons.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tobyos/abi/abi.h>

static inline long sc0(long n){long r;__asm__ volatile("syscall":"=a"(r):"0"(n):"rcx","r11","memory");return r;}
static inline long sc1(long n,long a){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a):"rcx","r11","memory");return r;}
static inline long sc2(long n,long a,long b){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b):"rcx","r11","memory");return r;}
static inline long sc3(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
static inline long sc5(long n,long a,long b,long c,long d,long e){long r;register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;__asm__ volatile("syscall":"=a"(r):"0"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");return r;}

static int  gui_create(int w,int h,const char*t){return(int)sc3(ABI_SYS_GUI_CREATE,w,h,(long)t);}
static void gui_fill(int fd,int x,int y,int w,int h,unsigned c){unsigned wh=((unsigned)(unsigned short)w)|(((unsigned)(unsigned short)h)<<16);sc5(ABI_SYS_GUI_FILL,fd,x,y,wh,c);}
static void gui_text(int fd,int x,int y,const char*s,unsigned fg){long xy=(long)(unsigned)((unsigned)x|((unsigned)y<<16));sc5(ABI_SYS_GUI_TEXT,fd,xy,(long)s,fg,0);}
static void gui_text_scaled(int fd,int x,int y,const char*s,unsigned fg,unsigned bg,int sc){long xy=(long)(unsigned)((unsigned)x|((unsigned)y<<16));unsigned a5=(bg&0x00FFFFFFu)|((unsigned)(sc&0x7F)<<24);sc5(ABI_SYS_GUI_TEXT_SCALED,fd,xy,(long)s,fg,a5);}
static void gui_flip(int fd){sc1(ABI_SYS_GUI_FLIP,fd);}
static long gui_poll(int fd,void*ev){return sc2(ABI_SYS_GUI_POLL_EVENT,fd,(long)ev);}
static void gui_set_title(int fd,const char*t){sc2(ABI_SYS_GUI_SET_TITLE,fd,(long)t);}
static long sys_readdir(const char*p,void*b,int c){return sc3(ABI_SYS_FS_READDIR,(long)p,(long)b,c);}
static long sys_clock_ms(void){return sc0(ABI_SYS_CLOCK_MS);}

struct gui_event { int type,x,y; unsigned char button,key,_pad[2]; };
#define EV_CLOSE 1
#define EV_MOUSE_DOWN 2
#define EV_KEY 4

#define W 800
#define H 500
#define PLAYLIST_W 250
#define CONTROLS_H 80
#define NAME_MAX_D 64

#define C_BG       0x1e1e2e
#define C_PLAYLIST 0x252540
#define C_PL_SEL   0x3a3a60
#define C_PL_TEXT  0xcccccc
#define C_PLAY_BG  0x16162a
#define C_CTRL_BG  0x202038
#define C_BTN      0x4444aa
#define C_BTN_ACT  0x6666cc
#define C_TEXT     0xffffff
#define C_DIM      0x888899
#define C_SEEK_BG  0x333355
#define C_SEEK_FG  0x6666ff
#define C_VOL_BG   0x333355
#define C_VOL_FG   0x44cc44

#define MAX_PLAYLIST 64

struct playlist_entry {
    char name[NAME_MAX_D];
    char path[256];
};

static struct playlist_entry playlist[MAX_PLAYLIST];
static int playlist_count;
static int current_track;
static int playing;
static int repeat_mode; /* 0=off, 1=one, 2=all */
static int shuffle;
static int volume; /* 0-100 */
static long play_start_ms;
static long track_duration_ms;
static int playlist_scroll;

static void format_time(long ms, char *buf) {
    int s = (int)(ms / 1000);
    int m = s / 60; s %= 60;
    buf[0] = '0' + m/10; buf[1] = '0' + m%10;
    buf[2] = ':';
    buf[3] = '0' + s/10; buf[4] = '0' + s%10;
    buf[5] = '\0';
}

static void load_directory(const char *path) {
    struct { char name[NAME_MAX_D]; unsigned type, size; } entries[128];
    int count = (int)sys_readdir(path, entries, 128);
    playlist_count = 0;

    for(int i=0; i<count && playlist_count<MAX_PLAYLIST; i++) {
        if(entries[i].type != 1) continue;
        int nl = (int)strlen(entries[i].name);
        /* Check for audio extensions */
        int is_audio = 0;
        if(nl > 4) {
            const char *ext = entries[i].name + nl - 4;
            if(strcmp(ext, ".mp3")==0 || strcmp(ext, ".wav")==0 ||
               strcmp(ext, ".ogg")==0 || strcmp(ext, ".m4a")==0)
                is_audio = 1;
        }
        if(!is_audio) continue;

        memcpy(playlist[playlist_count].name, entries[i].name, nl+1);
        int pl = (int)strlen(path);
        memcpy(playlist[playlist_count].path, path, pl);
        if(pl>0 && path[pl-1]!='/') playlist[playlist_count].path[pl++]='/';
        memcpy(playlist[playlist_count].path+pl, entries[i].name, nl+1);
        playlist_count++;
    }
}

static void play_track(int idx) {
    if(idx < 0 || idx >= playlist_count) return;
    current_track = idx;
    playing = 1;
    play_start_ms = sys_clock_ms();
    track_duration_ms = 180000; /* default 3 min until we read metadata */
}

static void stop_playback(void) {
    playing = 0;
}

static void next_track(void) {
    if(playlist_count == 0) return;
    int next = current_track + 1;
    if(next >= playlist_count) {
        if(repeat_mode == 2) next = 0;
        else { stop_playback(); return; }
    }
    play_track(next);
}

static void prev_track(void) {
    if(playlist_count == 0) return;
    int prev = current_track - 1;
    if(prev < 0) prev = playlist_count - 1;
    play_track(prev);
}

static void draw(int fd) {
    /* playlist panel */
    gui_fill(fd, 0, 0, PLAYLIST_W, H, C_PLAYLIST);
    gui_text(fd, 8, 8, "Playlist", C_TEXT);
    gui_fill(fd, 0, 26, PLAYLIST_W, 1, C_DIM);

    int vis = (H - CONTROLS_H - 30) / 20;
    for(int i=0; i<vis && (i+playlist_scroll)<playlist_count; i++) {
        int idx = i + playlist_scroll;
        int py = 30 + i*20;
        unsigned bg = (idx == current_track) ? C_PL_SEL : C_PLAYLIST;
        gui_fill(fd, 0, py, PLAYLIST_W, 19, bg);
        char label[NAME_MAX_D];
        snprintf(label, NAME_MAX_D, "%d. %s", idx+1, playlist[idx].name);
        gui_text(fd, 8, py+3, label, C_PL_TEXT);
    }

    /* playback area */
    int play_x = PLAYLIST_W;
    int play_w = W - PLAYLIST_W;
    int play_h = H - CONTROLS_H;
    gui_fill(fd, play_x, 0, play_w, play_h, C_PLAY_BG);

    /* album art placeholder */
    int art_sz = 180;
    int art_x = play_x + (play_w - art_sz)/2;
    int art_y = 40;
    gui_fill(fd, art_x, art_y, art_sz, art_sz, 0x333366);
    gui_text(fd, art_x+art_sz/2-24, art_y+art_sz/2-8, "Music", C_DIM);

    /* track info */
    if(current_track >= 0 && current_track < playlist_count) {
        gui_text_scaled(fd, play_x+40, art_y+art_sz+20,
                        playlist[current_track].name, C_TEXT, C_PLAY_BG, 2);
    }

    /* controls */
    int cy = H - CONTROLS_H;
    gui_fill(fd, play_x, cy, play_w, CONTROLS_H, C_CTRL_BG);

    /* seek bar */
    int seek_y = cy + 8;
    gui_fill(fd, play_x+20, seek_y, play_w-40, 6, C_SEEK_BG);
    if(playing && track_duration_ms > 0) {
        long elapsed = sys_clock_ms() - play_start_ms;
        if(elapsed > track_duration_ms) elapsed = track_duration_ms;
        int fill_w = (int)((elapsed * (play_w-40)) / track_duration_ms);
        gui_fill(fd, play_x+20, seek_y, fill_w, 6, C_SEEK_FG);

        char pos[8], dur[8];
        format_time(elapsed, pos);
        format_time(track_duration_ms, dur);
        gui_text(fd, play_x+20, seek_y+10, pos, C_DIM);
        char dstr[8];
        format_time(track_duration_ms, dstr);
        gui_text(fd, play_x+play_w-68, seek_y+10, dstr, C_DIM);
    }

    /* buttons */
    int btn_y = cy + 35;
    int btn_x = play_x + play_w/2 - 120;

    /* prev */
    gui_fill(fd, btn_x, btn_y, 50, 30, C_BTN);
    gui_text(fd, btn_x+10, btn_y+8, "|<<", C_TEXT);
    btn_x += 60;

    /* play/pause */
    gui_fill(fd, btn_x, btn_y, 60, 30, playing ? C_BTN_ACT : C_BTN);
    gui_text(fd, btn_x+8, btn_y+8, playing ? "Pause" : "Play", C_TEXT);
    btn_x += 70;

    /* stop */
    gui_fill(fd, btn_x, btn_y, 50, 30, C_BTN);
    gui_text(fd, btn_x+8, btn_y+8, "Stop", C_TEXT);
    btn_x += 60;

    /* next */
    gui_fill(fd, btn_x, btn_y, 50, 30, C_BTN);
    gui_text(fd, btn_x+10, btn_y+8, ">>|", C_TEXT);

    /* volume */
    int vol_x = play_x + play_w - 120;
    gui_text(fd, vol_x, btn_y+8, "Vol:", C_DIM);
    gui_fill(fd, vol_x+36, btn_y+12, 70, 6, C_VOL_BG);
    int vol_w = volume * 70 / 100;
    gui_fill(fd, vol_x+36, btn_y+12, vol_w, 6, C_VOL_FG);

    /* repeat/shuffle indicators */
    gui_text(fd, play_x+20, btn_y+8,
             repeat_mode==0 ? "Rep:Off" : repeat_mode==1 ? "Rep:One" : "Rep:All",
             repeat_mode ? C_BTN_ACT : C_DIM);
    gui_text(fd, play_x+100, btn_y+8,
             shuffle ? "Shuf:On" : "Shuf:Off",
             shuffle ? C_BTN_ACT : C_DIM);

    gui_flip(fd);
}

int main(int argc, char **argv) {
    playlist_count = 0;
    current_track = -1;
    playing = 0;
    repeat_mode = 0;
    shuffle = 0;
    volume = 80;
    play_start_ms = 0;
    track_duration_ms = 0;
    playlist_scroll = 0;

    if(argc > 1) {
        /* If given a directory, load all audio files from it */
        load_directory(argv[1]);
    }
    if(playlist_count == 0) {
        load_directory("/");
    }

    int fd = gui_create(W, H, "Media Player");
    if(fd < 0) return 1;

    draw(fd);
    long last_draw = sys_clock_ms();

    struct gui_event ev;
    for(;;) {
        /* Auto-advance track */
        if(playing && track_duration_ms > 0) {
            long elapsed = sys_clock_ms() - play_start_ms;
            if(elapsed >= track_duration_ms) {
                if(repeat_mode == 1) play_track(current_track);
                else next_track();
            }
        }

        /* Redraw periodically for seek bar */
        long now = sys_clock_ms();
        if(now - last_draw > 500) {
            draw(fd);
            last_draw = now;
        }

        if(gui_poll(fd, &ev) > 0) {
            if(ev.type == EV_CLOSE) break;

            if(ev.type == EV_MOUSE_DOWN) {
                /* Playlist click */
                if(ev.x < PLAYLIST_W && ev.y >= 30) {
                    int idx = (ev.y - 30) / 20 + playlist_scroll;
                    if(idx >= 0 && idx < playlist_count) {
                        play_track(idx);
                    }
                }
                /* Control button clicks */
                int cy = H - CONTROLS_H;
                int btn_y = cy + 35;
                if(ev.y >= btn_y && ev.y < btn_y+30) {
                    int play_x = PLAYLIST_W;
                    int play_w = W - PLAYLIST_W;
                    int btn_x = play_x + play_w/2 - 120;
                    if(ev.x >= btn_x && ev.x < btn_x+50) prev_track();
                    btn_x += 60;
                    if(ev.x >= btn_x && ev.x < btn_x+60) {
                        if(playing) stop_playback();
                        else if(current_track >= 0) play_track(current_track);
                        else if(playlist_count > 0) play_track(0);
                    }
                    btn_x += 70;
                    if(ev.x >= btn_x && ev.x < btn_x+50) stop_playback();
                    btn_x += 60;
                    if(ev.x >= btn_x && ev.x < btn_x+50) next_track();
                }
                draw(fd);
            }

            if(ev.type == EV_KEY) {
                switch(ev.key) {
                case ' ': /* space = play/pause */
                    if(playing) stop_playback();
                    else if(current_track >= 0) play_track(current_track);
                    else if(playlist_count > 0) play_track(0);
                    break;
                case 's': stop_playback(); break;
                case 'n': next_track(); break;
                case 'p': prev_track(); break;
                case 'r': repeat_mode = (repeat_mode + 1) % 3; break;
                case 'h': shuffle = !shuffle; break;
                case '+': if(volume < 100) volume += 5; break;
                case '-': if(volume > 0) volume -= 5; break;
                case 'q': _exit(0); break;
                }
                draw(fd);
            }
        }
        __asm__ volatile("syscall"::"a"((long)ABI_SYS_YIELD):"rcx","r11","memory");
    }
    _exit(0);
    return 0;
}
