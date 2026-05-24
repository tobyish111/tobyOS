/* calc/main.c -- Calculator for tobyOS (Milestone 5).
 *
 * 300x400 GUI window with display and button grid. Fixed-point
 * arithmetic (SCALE=10000 for 4 decimal places). Supports basic
 * arithmetic, memory, percent, negate, keyboard input.
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

struct gui_event { int type,x,y; unsigned char button,key,_pad[2]; };
#define EV_CLOSE 1
#define EV_MOUSE_DOWN 2
#define EV_KEY 4

#define WIN_W 300
#define WIN_H 400
#define DISPLAY_H 60
#define BTN_ROWS 6
#define BTN_COLS 4
#define BTN_W (WIN_W / BTN_COLS)
#define BTN_H ((WIN_H - DISPLAY_H) / BTN_ROWS)

#define C_BG       0x2d2d2d
#define C_DISPLAY  0x1a1a1a
#define C_DISP_FG  0xffffff
#define C_BTN_NUM  0x3d3d3d
#define C_BTN_OP   0x505050
#define C_BTN_EQ   0xff9500
#define C_BTN_FN   0x646464
#define C_BTN_MEM  0x404040
#define C_BTN_TEXT 0xffffff
#define C_BTN_EQT  0xffffff

#define SCALE 10000LL

static long long display_val;
static long long accumulator;
static int  pending_op; /* '+' '-' '*' '/' */
static int  new_input;
static int  has_decimal;
static int  decimal_digits;
static long long memory;
static char display_str[32];

static void val_to_str(long long val, char *buf) {
    int neg = 0;
    if(val < 0) { neg = 1; val = -val; }

    long long whole = val / SCALE;
    long long frac = val % SCALE;

    int pos = 0;
    if(neg) buf[pos++] = '-';

    /* whole part */
    char tmp[20]; int tc = 0;
    if(whole == 0) tmp[tc++] = '0';
    while(whole > 0) { tmp[tc++] = '0' + (int)(whole%10); whole /= 10; }
    for(int i=tc-1; i>=0; i--) buf[pos++] = tmp[i];

    /* fractional part (only if non-zero) */
    if(frac > 0) {
        buf[pos++] = '.';
        char fb[5];
        fb[0] = '0' + (int)((frac/1000)%10);
        fb[1] = '0' + (int)((frac/100)%10);
        fb[2] = '0' + (int)((frac/10)%10);
        fb[3] = '0' + (int)(frac%10);
        fb[4] = '\0';
        /* trim trailing zeros */
        int end = 3;
        while(end > 0 && fb[end] == '0') end--;
        for(int i=0; i<=end; i++) buf[pos++] = fb[i];
    }
    buf[pos] = '\0';
}

static void update_display(void) {
    val_to_str(display_val, display_str);
}

static void do_operation(void) {
    switch(pending_op) {
    case '+': accumulator += display_val; break;
    case '-': accumulator -= display_val; break;
    case '*': accumulator = (accumulator * display_val) / SCALE; break;
    case '/':
        if(display_val != 0)
            accumulator = (accumulator * SCALE) / display_val;
        break;
    default: accumulator = display_val; break;
    }
}

struct btn_def {
    const char *label;
    char  action;
    unsigned color;
};

static const struct btn_def buttons[BTN_ROWS][BTN_COLS] = {
    {{"MC",'G',C_BTN_MEM}, {"MR",'R',C_BTN_MEM}, {"M+",'P',C_BTN_MEM}, {"M-",'N',C_BTN_MEM}},
    {{"C" ,'C',C_BTN_FN},  {"CE",'E',C_BTN_FN},  {"+/-",'~',C_BTN_FN}, {"%" ,'%',C_BTN_OP}},
    {{"7" ,'7',C_BTN_NUM}, {"8" ,'8',C_BTN_NUM}, {"9" ,'9',C_BTN_NUM}, {"/" ,'/',C_BTN_OP}},
    {{"4" ,'4',C_BTN_NUM}, {"5" ,'5',C_BTN_NUM}, {"6" ,'6',C_BTN_NUM}, {"*" ,'*',C_BTN_OP}},
    {{"1" ,'1',C_BTN_NUM}, {"2" ,'2',C_BTN_NUM}, {"3" ,'3',C_BTN_NUM}, {"-" ,'-',C_BTN_OP}},
    {{"0" ,'0',C_BTN_NUM}, {"." ,'.',C_BTN_NUM}, {"=" ,'=',C_BTN_EQ},  {"+" ,'+',C_BTN_OP}},
};

static void draw(int fd) {
    /* display */
    gui_fill(fd, 0, 0, WIN_W, DISPLAY_H, C_DISPLAY);
    update_display();
    int tlen = (int)strlen(display_str);
    int tx = WIN_W - 12 - tlen * 16;
    if(tx < 4) tx = 4;
    gui_text_scaled(fd, tx, 16, display_str, C_DISP_FG, C_DISPLAY, 2);

    /* buttons */
    for(int r=0; r<BTN_ROWS; r++) {
        for(int c=0; c<BTN_COLS; c++) {
            int bx = c * BTN_W;
            int by = DISPLAY_H + r * BTN_H;
            unsigned bg = buttons[r][c].color;
            gui_fill(fd, bx+1, by+1, BTN_W-2, BTN_H-2, bg);

            const char *label = buttons[r][c].label;
            int llen = (int)strlen(label);
            int lx = bx + (BTN_W - llen*8) / 2;
            int ly = by + (BTN_H - 16) / 2;
            unsigned fg = (bg == C_BTN_EQ) ? C_BTN_EQT : C_BTN_TEXT;
            gui_text(fd, lx, ly, label, fg);
        }
    }
    gui_flip(fd);
}

static void handle_action(char action) {
    if(action >= '0' && action <= '9') {
        if(new_input) { display_val = 0; new_input = 0; has_decimal = 0; decimal_digits = 0; }
        int digit = action - '0';
        if(has_decimal) {
            if(decimal_digits < 4) {
                decimal_digits++;
                long long place = SCALE;
                for(int i=0; i<decimal_digits; i++) place /= 10;
                if(display_val >= 0) display_val += digit * place;
                else display_val -= digit * place;
            }
        } else {
            if(display_val >= 0)
                display_val = display_val * 10 + digit * SCALE;
            else
                display_val = display_val * 10 - digit * SCALE;
        }
        return;
    }

    switch(action) {
    case '.':
        if(new_input) { display_val = 0; new_input = 0; }
        has_decimal = 1;
        break;
    case '+': case '-': case '*': case '/':
        do_operation();
        display_val = accumulator;
        pending_op = action;
        new_input = 1;
        has_decimal = 0;
        decimal_digits = 0;
        break;
    case '=':
        do_operation();
        display_val = accumulator;
        pending_op = 0;
        new_input = 1;
        has_decimal = 0;
        decimal_digits = 0;
        break;
    case 'C': /* clear all */
        display_val = 0; accumulator = 0; pending_op = 0;
        new_input = 1; has_decimal = 0; decimal_digits = 0;
        break;
    case 'E': /* clear entry */
        display_val = 0; new_input = 0; has_decimal = 0; decimal_digits = 0;
        break;
    case '~': /* negate */
        display_val = -display_val;
        break;
    case '%':
        display_val = (accumulator * display_val) / (100 * SCALE);
        new_input = 1;
        break;
    case 'G': memory = 0; break;             /* MC */
    case 'R': display_val = memory; new_input = 1; break; /* MR */
    case 'P': memory += display_val; break;   /* M+ */
    case 'N': memory -= display_val; break;   /* M- */
    }
}

int main(void) {
    display_val = 0;
    accumulator = 0;
    pending_op = 0;
    new_input = 1;
    has_decimal = 0;
    decimal_digits = 0;
    memory = 0;

    int fd = gui_create(WIN_W, WIN_H, "Calculator");
    if(fd < 0) return 1;

    draw(fd);

    struct gui_event ev;
    for(;;) {
        if(gui_poll(fd, &ev) > 0) {
            if(ev.type == EV_CLOSE) break;

            if(ev.type == EV_MOUSE_DOWN) {
                if(ev.y >= DISPLAY_H) {
                    int r = (ev.y - DISPLAY_H) / BTN_H;
                    int c = ev.x / BTN_W;
                    if(r >= 0 && r < BTN_ROWS && c >= 0 && c < BTN_COLS) {
                        handle_action(buttons[r][c].action);
                        draw(fd);
                    }
                }
            }

            if(ev.type == EV_KEY) {
                char k = (char)ev.key;
                if(k >= '0' && k <= '9') handle_action(k);
                else if(k == '+' || k == '-' || k == '*' || k == '/') handle_action(k);
                else if(k == '=' || k == '\n' || k == '\r') handle_action('=');
                else if(k == '.') handle_action('.');
                else if(k == 0x08) handle_action('E');
                else if(k == 0x1b) handle_action('C');
                else if(k == '%') handle_action('%');
                draw(fd);
            }
        }
        __asm__ volatile("syscall"::"a"((long)ABI_SYS_YIELD):"rcx","r11","memory");
    }
    _exit(0);
    return 0;
}
