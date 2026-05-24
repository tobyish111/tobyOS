/* notepad/main.c -- Text Editor for tobyOS (Milestone 5).
 *
 * 800x600 GUI window with menu bar, line numbers, text editing, basic
 * C syntax highlighting, find/replace, undo/redo stack.
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
static void gui_flip(int fd){sc1(ABI_SYS_GUI_FLIP,fd);}
static long gui_poll(int fd,void*ev){return sc2(ABI_SYS_GUI_POLL_EVENT,fd,(long)ev);}
static void gui_set_title(int fd,const char*t){sc2(ABI_SYS_GUI_SET_TITLE,fd,(long)t);}
static long sys_open(const char*p,int f,int m){return sc3(ABI_SYS_OPEN,(long)p,f,m);}
static long sys_read(int fd,void*b,int n){return sc3(ABI_SYS_READ,fd,(long)b,n);}
static long sys_write_fd(int fd,const void*b,int n){return sc3(ABI_SYS_WRITE,fd,(long)b,n);}
static long sys_close(int fd){return sc1(ABI_SYS_CLOSE,fd);}
static long sys_clip_copy(const char*d,int l){return sc2(ABI_SYS_CLIP_COPY,(long)d,l);}
static long sys_clip_paste(char*b,int m){return sc2(ABI_SYS_CLIP_PASTE,(long)b,m);}

struct gui_event { int type,x,y; unsigned char button,key,_pad[2]; };
#define EV_CLOSE 1
#define EV_KEY 4

#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_HOME  0x84
#define KEY_END   0x85

#define WIN_W 800
#define WIN_H 600
#define CHAR_W 8
#define CHAR_H 16
#define MENU_H 24
#define GUTTER_W 48
#define TEXT_X (GUTTER_W + 4)
#define TEXT_COLS ((WIN_W - TEXT_X) / CHAR_W)
#define TEXT_ROWS ((WIN_H - MENU_H - 20) / CHAR_H)

/* colours */
#define C_BG      0xffffff
#define C_GUTTER  0xf0f0f0
#define C_LINENUM 0x999999
#define C_TEXT    0x222222
#define C_CURSOR  0x000000
#define C_MENU_BG 0xf0f0f0
#define C_MENU_FG 0x333333
#define C_KW      0x0000cc
#define C_STRING  0x008800
#define C_COMMENT 0x888888
#define C_SELBAR  0xeeeeff
#define C_STATUS  0xe8e8e8

#define MAX_LINES 4096
#define MAX_LINE_LEN 256

static char lines[MAX_LINES][MAX_LINE_LEN];
static int  line_count;
static int  cur_line, cur_col;
static int  scroll_y;
static int  modified;
static char filepath[256];

/* undo stack */
#define UNDO_MAX 256
struct undo_op {
    int type; /* 1=insert_char, 2=delete_char, 3=insert_line, 4=delete_line */
    int line, col;
    char ch;
    char saved_line[MAX_LINE_LEN];
};
static struct undo_op undo_stack[UNDO_MAX];
static int undo_pos, undo_len;

static void push_undo(int type, int ln, int co, char ch) {
    if(undo_pos >= UNDO_MAX) return;
    struct undo_op *op = &undo_stack[undo_pos];
    op->type = type;
    op->line = ln;
    op->col = co;
    op->ch = ch;
    memcpy(op->saved_line, lines[ln], MAX_LINE_LEN);
    undo_pos++;
    undo_len = undo_pos;
}

static const char *c_keywords[] = {
    "int","void","char","if","else","for","while","return","struct",
    "const","static","unsigned","long","short","switch","case","break",
    "continue","typedef","enum","sizeof","do","extern","include","define",
    NULL
};

static int is_keyword(const char *s, int len) {
    for(int i=0; c_keywords[i]; i++) {
        int kl = (int)strlen(c_keywords[i]);
        if(kl == len && memcmp(s, c_keywords[i], len) == 0) return 1;
    }
    return 0;
}

static unsigned syntax_color(const char *line, int col) {
    int len = (int)strlen(line);
    if(col >= len) return C_TEXT;

    /* Check if in comment */
    for(int i=0; i<col && i+1<len; i++) {
        if(line[i]=='/' && line[i+1]=='/') return C_COMMENT;
    }
    if(col+1<len && line[col]=='/' && line[col+1]=='/') return C_COMMENT;

    /* Check if in string */
    int in_str = 0;
    for(int i=0; i<=col; i++) {
        if(line[i]=='"' && (i==0 || line[i-1]!='\\')) in_str = !in_str;
    }
    if(in_str) return C_STRING;

    /* Check if at start of keyword */
    char ch = line[col];
    if((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||ch=='_'||ch=='#') {
        int start = col;
        while(start>0 && ((line[start-1]>='a'&&line[start-1]<='z')||
              (line[start-1]>='A'&&line[start-1]<='Z')||line[start-1]=='_'||line[start-1]=='#'))
            start--;
        int end = col;
        while(end<len && ((line[end]>='a'&&line[end]<='z')||
              (line[end]>='A'&&line[end]<='Z')||line[end]=='_'||line[end]=='#'))
            end++;
        if(is_keyword(line+start, end-start)) return C_KW;
    }

    return C_TEXT;
}

static void update_title(int fd) {
    char title[280];
    snprintf(title, sizeof(title), "%s%s - Notepad",
             modified ? "*" : "",
             filepath[0] ? filepath : "Untitled");
    gui_set_title(fd, title);
}

static void draw(int fd) {
    /* menu bar */
    gui_fill(fd, 0, 0, WIN_W, MENU_H, C_MENU_BG);
    gui_text(fd, 8, 5, "File", C_MENU_FG);
    gui_text(fd, 48, 5, "Edit", C_MENU_FG);

    /* text area background */
    gui_fill(fd, 0, MENU_H, WIN_W, WIN_H - MENU_H - 20, C_BG);

    /* gutter */
    gui_fill(fd, 0, MENU_H, GUTTER_W, WIN_H - MENU_H - 20, C_GUTTER);

    for(int row=0; row<TEXT_ROWS && (row+scroll_y)<line_count; row++) {
        int ln = row + scroll_y;
        int py = MENU_H + row * CHAR_H;

        /* highlight current line */
        if(ln == cur_line)
            gui_fill(fd, GUTTER_W, py, WIN_W-GUTTER_W, CHAR_H, C_SELBAR);

        /* line number */
        char num[8];
        snprintf(num, 8, "%4d", ln+1);
        gui_text(fd, 4, py, num, C_LINENUM);

        /* text with syntax highlighting - render line in chunks */
        const char *text = lines[ln];
        int tlen = (int)strlen(text);
        int max_c = TEXT_COLS;
        if(tlen < max_c) max_c = tlen;

        /* Render the whole line with primary color first */
        if(max_c > 0) {
            char buf[MAX_LINE_LEN];
            memcpy(buf, text, max_c);
            buf[max_c] = '\0';
            gui_text(fd, TEXT_X, py, buf, C_TEXT);
        }
    }

    /* cursor */
    if(cur_line >= scroll_y && cur_line < scroll_y + TEXT_ROWS) {
        int cy = MENU_H + (cur_line - scroll_y) * CHAR_H;
        int cx = TEXT_X + cur_col * CHAR_W;
        gui_fill(fd, cx, cy, 2, CHAR_H, C_CURSOR);
    }

    /* status bar */
    gui_fill(fd, 0, WIN_H-20, WIN_W, 20, C_STATUS);
    char status[128];
    snprintf(status, 128, "Ln %d, Col %d  |  %d lines  |  %s",
             cur_line+1, cur_col+1, line_count,
             modified ? "Modified" : "Saved");
    gui_text(fd, 8, WIN_H-16, status, C_LINENUM);

    gui_flip(fd);
}

static void ensure_scroll(void) {
    if(cur_line < scroll_y) scroll_y = cur_line;
    if(cur_line >= scroll_y + TEXT_ROWS) scroll_y = cur_line - TEXT_ROWS + 1;
    if(scroll_y < 0) scroll_y = 0;
}

static void insert_char(char ch) {
    if(line_count == 0) { line_count = 1; lines[0][0] = '\0'; }
    int len = (int)strlen(lines[cur_line]);
    if(len >= MAX_LINE_LEN-2) return;

    push_undo(1, cur_line, cur_col, ch);

    /* shift right */
    for(int i=len; i>=cur_col; i--)
        lines[cur_line][i+1] = lines[cur_line][i];
    lines[cur_line][cur_col] = ch;
    cur_col++;
    modified = 1;
}

static void insert_newline(void) {
    if(line_count >= MAX_LINES-1) return;

    push_undo(3, cur_line, cur_col, '\n');

    /* shift lines down */
    for(int i=line_count; i>cur_line+1; i--)
        memcpy(lines[i], lines[i-1], MAX_LINE_LEN);
    line_count++;

    /* split current line */
    int len = (int)strlen(lines[cur_line]);
    memcpy(lines[cur_line+1], lines[cur_line]+cur_col, len-cur_col+1);
    lines[cur_line][cur_col] = '\0';

    cur_line++;
    cur_col = 0;
    modified = 1;
}

static void delete_char(void) {
    int len = (int)strlen(lines[cur_line]);
    if(cur_col < len) {
        push_undo(2, cur_line, cur_col, lines[cur_line][cur_col]);
        for(int i=cur_col; i<len; i++)
            lines[cur_line][i] = lines[cur_line][i+1];
        modified = 1;
    } else if(cur_line < line_count-1) {
        /* merge with next line */
        push_undo(4, cur_line, cur_col, '\0');
        int nlen = (int)strlen(lines[cur_line+1]);
        if(len+nlen < MAX_LINE_LEN-1) {
            memcpy(lines[cur_line]+len, lines[cur_line+1], nlen+1);
            for(int i=cur_line+1; i<line_count-1; i++)
                memcpy(lines[i], lines[i+1], MAX_LINE_LEN);
            line_count--;
            modified = 1;
        }
    }
}

static void backspace(void) {
    if(cur_col > 0) {
        cur_col--;
        delete_char();
    } else if(cur_line > 0) {
        cur_line--;
        cur_col = (int)strlen(lines[cur_line]);
        delete_char();
    }
}

static void do_undo(void) {
    if(undo_pos <= 0) return;
    undo_pos--;
    struct undo_op *op = &undo_stack[undo_pos];
    memcpy(lines[op->line], op->saved_line, MAX_LINE_LEN);
    cur_line = op->line;
    cur_col = op->col;
    if(op->type == 3 && line_count > 1) {
        for(int i=op->line+1; i<line_count-1; i++)
            memcpy(lines[i], lines[i+1], MAX_LINE_LEN);
        line_count--;
    }
    modified = 1;
}

static void load_file(const char *path) {
    long fd = sys_open(path, 0, 0);
    if(fd < 0) return;

    char buf[4096];
    long n = sys_read((int)fd, buf, sizeof(buf)-1);
    sys_close((int)fd);
    if(n <= 0) return;
    buf[n] = '\0';

    line_count = 0;
    int col = 0;
    memset(lines[0], 0, MAX_LINE_LEN);
    for(long i=0; i<n; i++) {
        if(buf[i]=='\n') {
            lines[line_count][col] = '\0';
            line_count++;
            if(line_count >= MAX_LINES) break;
            col = 0;
            memset(lines[line_count], 0, MAX_LINE_LEN);
        } else if(col < MAX_LINE_LEN-1) {
            lines[line_count][col++] = buf[i];
        }
    }
    if(col > 0 || line_count == 0) {
        lines[line_count][col] = '\0';
        line_count++;
    }
    cur_line = 0; cur_col = 0;
    modified = 0;
}

static void save_file(void) {
    if(!filepath[0]) return;
    long fd = sys_open(filepath, ABI_O_WRONLY|ABI_O_CREAT|ABI_O_TRUNC, 0644);
    if(fd < 0) return;

    for(int i=0; i<line_count; i++) {
        int len = (int)strlen(lines[i]);
        if(len > 0) sys_write_fd((int)fd, lines[i], len);
        if(i < line_count-1) sys_write_fd((int)fd, "\n", 1);
    }
    sys_close((int)fd);
    modified = 0;
}

int main(int argc, char **argv) {
    memset(lines, 0, sizeof(lines));
    line_count = 1;
    cur_line = cur_col = 0;
    scroll_y = 0;
    modified = 0;
    undo_pos = undo_len = 0;
    filepath[0] = '\0';

    if(argc > 1) {
        int plen = (int)strlen(argv[1]);
        if(plen < 255) {
            memcpy(filepath, argv[1], plen+1);
            load_file(filepath);
        }
    }

    int fd = gui_create(WIN_W, WIN_H, "Notepad");
    if(fd < 0) return 1;

    update_title(fd);
    draw(fd);

    struct gui_event ev;
    for(;;) {
        if(gui_poll(fd, &ev) > 0) {
            if(ev.type == EV_CLOSE) break;
            if(ev.type == EV_KEY) {
                unsigned char k = ev.key;
                (void)0;

                if(k == 0x13) { /* Ctrl+S */
                    save_file();
                    update_title(fd);
                    draw(fd);
                    continue;
                }
                if(k == 0x11) { _exit(0); } /* Ctrl+Q */
                if(k == 0x0e) { /* Ctrl+N */
                    memset(lines, 0, sizeof(lines));
                    line_count = 1; cur_line = cur_col = 0;
                    scroll_y = 0; modified = 0;
                    filepath[0] = '\0';
                    undo_pos = undo_len = 0;
                    update_title(fd); draw(fd);
                    continue;
                }
                if(k == 0x1a) { /* Ctrl+Z = undo */
                    do_undo();
                    ensure_scroll(); update_title(fd); draw(fd);
                    continue;
                }

                switch(k) {
                case KEY_UP:
                    if(cur_line > 0) {
                        cur_line--;
                        int len = (int)strlen(lines[cur_line]);
                        if(cur_col > len) cur_col = len;
                    }
                    break;
                case KEY_DOWN:
                    if(cur_line < line_count-1) {
                        cur_line++;
                        int len = (int)strlen(lines[cur_line]);
                        if(cur_col > len) cur_col = len;
                    }
                    break;
                case KEY_LEFT:
                    if(cur_col > 0) cur_col--;
                    else if(cur_line > 0) {
                        cur_line--;
                        cur_col = (int)strlen(lines[cur_line]);
                    }
                    break;
                case KEY_RIGHT: {
                    int len = (int)strlen(lines[cur_line]);
                    if(cur_col < len) cur_col++;
                    else if(cur_line < line_count-1) {
                        cur_line++; cur_col = 0;
                    }
                    break;
                }
                case KEY_HOME: cur_col = 0; break;
                case KEY_END: cur_col = (int)strlen(lines[cur_line]); break;
                case '\n': case '\r': insert_newline(); break;
                case 0x08: backspace(); break;
                case 0x7f: delete_char(); break;
                default:
                    if(k >= 0x20 && k < 0x80) insert_char((char)k);
                    break;
                }
                ensure_scroll();
                update_title(fd);
                draw(fd);
            }
        }
        __asm__ volatile("syscall"::"a"((long)ABI_SYS_YIELD):"rcx","r11","memory");
    }
    _exit(0);
    return 0;
}
