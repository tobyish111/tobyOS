/* programs/linux-evdev/main.c
 *
 * A DYNAMICALLY-LINKED Linux x86-64 app that reads the keyboard through the
 * genuine Linux input layer: /dev/input/event0 (evdev). It is a musl PIE
 * (PT_INTERP=/lib/ld-musl-x86_64.so.1), so the kernel loads it through the
 * real ld-musl loader and every call below is a real libc call.
 *
 * It is a faithful evdev client: it open()s the device, queries identity with
 * EVIOCGVERSION / EVIOCGNAME, then read()s fixed-size `struct input_event`
 * records in a loop, collecting EV_KEY *press* events until it sees ENTER.
 * The boot harness injects the keycodes for T, O, B, Y, ENTER, so a correct
 * decode yields "TOBY" and the program exits 0.
 *
 * No tobyOS headers are used; the structures and ioctl encodings below are the
 * verbatim Linux <linux/input.h> / <sys/ioctl.h> UAPI.
 */

typedef unsigned long size_t;
typedef long ssize_t;
typedef struct _TOBY_FILE FILE;
extern FILE *const stdout;
extern FILE *const stderr;

int   open(const char *, int, ...);
int   close(int);
int   ioctl(int, unsigned long, ...);
ssize_t read(int, void *, size_t);
int   printf(const char *, ...);
int   fprintf(FILE *, const char *, ...);
int   fflush(FILE *);

#define O_RDONLY 0

/* ---- verbatim <linux/input.h> UAPI --------------------------------- */
struct timeval { long tv_sec; long tv_usec; };
struct input_event {
    struct timeval time;
    unsigned short type;
    unsigned short code;
    int            value;
};

#define EV_SYN  0x00
#define EV_KEY  0x01

/* ioctl encoding (verbatim <asm-generic/ioctl.h>) */
#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  8
#define _IOC_SIZESHIFT  16
#define _IOC_DIRSHIFT   30
#define _IOC_READ       2u
#define _IOC(dir,type,nr,size) \
    (((unsigned long)(dir)  << _IOC_DIRSHIFT)  | \
     ((unsigned long)(type) << _IOC_TYPESHIFT) | \
     ((unsigned long)(nr)   << _IOC_NRSHIFT)   | \
     ((unsigned long)(size) << _IOC_SIZESHIFT))
#define EVIOCGVERSION       _IOC(_IOC_READ, 'E', 0x01, 4)
#define EVIOCGNAME(len)     _IOC(_IOC_READ, 'E', 0x06, (len))

/* Linux keycode -> ASCII for the handful of letter keys we care about. */
static char keycode_to_ascii(unsigned short code) {
    switch (code) {
    case 20: return 'T';
    case 24: return 'O';
    case 48: return 'B';
    case 21: return 'Y';
    case 28: return '\n';   /* ENTER */
    default: return '?';
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    int fd = open("/dev/input/event0", O_RDONLY);
    if (fd < 0) { fprintf(stderr, "evdev: open(/dev/input/event0) failed\n"); return 1; }

    int version = 0;
    char name[64] = { 0 };
    ioctl(fd, EVIOCGVERSION, &version);
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    printf("evdev: '%s' driver v%d.%d.%d (dynamic musl libc via ld-musl)\n",
           name[0] ? name : "?", (version >> 16) & 0xff,
           (version >> 8) & 0xff, version & 0xff);
    fflush(stdout);

    char typed[16];
    int  n = 0;
    struct input_event evs[32];
    int done = 0;

    while (!done && n < (int)sizeof(typed) - 1) {
        ssize_t r = read(fd, evs, sizeof(evs));
        if (r <= 0) break;
        int count = (int)(r / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < count; i++) {
            if (evs[i].type != EV_KEY || evs[i].value != 1) continue; /* presses only */
            char c = keycode_to_ascii(evs[i].code);
            if (c == '\n') { done = 1; break; }      /* ENTER terminates */
            typed[n++] = c;
        }
    }
    typed[n] = 0;
    close(fd);

    printf("evdev: decoded \"%s\" from %d EV_KEY press events\n", typed, n);
    fflush(stdout);

    /* Success iff we decoded exactly the injected word. */
    const char *want = "TOBY";
    for (int i = 0; i < 5; i++)
        if (typed[i] != want[i]) return 5;   /* compares through the NUL too */
    return 0;
}
