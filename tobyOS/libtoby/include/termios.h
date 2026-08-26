/* termios.h -- libtoby's terminal I/O control.
 *
 * THE VALUES BELOW ARE THE LINUX x86-64 ONES, AND THAT IS NOT COSMETIC.
 * tcgetattr/tcsetattr exchange this struct with the kernel through
 * TCGETS/TCSETS (0x5401/0x5402), so every field offset and every bit
 * here has to agree with `struct ktermios` in include/tobyos/tty.h.
 *
 * They did not agree, and nothing noticed for a long time because
 * tcsetattr never reached the kernel at all -- see libtoby/src/termios.c.
 * Once it did, three separate disagreements would each have corrupted
 * the result:
 *
 *   1. `c_line` WAS MISSING from this struct, so every c_cc index was
 *      shifted one byte against what the kernel reads.
 *   2. NCCS was 20 against the kernel ABI's 19.
 *   3. the flag bits were invented (ICANON 0x0010, ECHO 0x0001,
 *      ICRNL 0x0001) rather than Linux's (0x0002, 0x0008, 0x0100), and
 *      the c_cc indices likewise (VMIN was 5, which is Linux's VTIME).
 *
 * Symptom of all that together: a program that asked for raw mode got a
 * terminal still in CANONICAL mode with ECHO on -- line-buffered input,
 * ^U killing the pending line, DEL erasing, ^D delivering EOF, CR
 * translated to LF. An editor cannot work on such a terminal.
 */

#ifndef LIBTOBY_TERMIOS_H
#define LIBTOBY_TERMIOS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

/* 19, matching the kernel ABI's struct ktermios. */
#define NCCS 19

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;            /* MUST be here: the kernel has it */
    cc_t     c_cc[NCCS];
    /* Past the 36 bytes the kernel exchanges. Userspace-only, so the
     * cf*speed helpers have somewhere to live without disturbing the
     * ABI-visible prefix above. */
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

/* c_iflag bits (Linux). */
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IUCLC   0x0200
#define IXON    0x0400
#define IXANY   0x0800
#define IXOFF   0x1000

/* c_oflag bits (Linux). */
#define OPOST   0x0001
#define OLCUC   0x0002
#define ONLCR   0x0004
#define OCRNL   0x0008
#define ONOCR   0x0010
#define ONLRET  0x0020

/* c_cflag bits (Linux). */
#define CSIZE   0x0030
#define CS5     0x0000
#define CS6     0x0010
#define CS7     0x0020
#define CS8     0x0030
#define CSTOPB  0x0040
#define CREAD   0x0080
#define PARENB  0x0100
#define PARODD  0x0200
#define HUPCL   0x0400
#define CLOCAL  0x0800

/* c_lflag bits (Linux). */
#define ISIG    0x0001
#define ICANON  0x0002
#define XCASE   0x0004
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define ECHOCTL 0x0200
#define ECHOPRT 0x0400
#define ECHOKE  0x0800
#define FLUSHO  0x1000
#define PENDIN  0x4000
#define IEXTEN  0x8000

/* c_cc indices (Linux). VMIN is 6 and VTIME is 5 -- they are NOT
 * adjacent in the order a reader might guess, and swapping them makes a
 * raw read block forever or spin. */
#define VINTR     0
#define VQUIT     1
#define VERASE    2
#define VKILL     3
#define VEOF      4
#define VTIME     5
#define VMIN      6
#define VSWTC     7
#define VSTART    8
#define VSTOP     9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

/* tcsetattr actions (Linux). */
#define TCSANOW    0
#define TCSADRAIN  1
#define TCSAFLUSH  2

/* tcflush queue selectors (Linux). */
#define TCIFLUSH   0
#define TCOFLUSH   1
#define TCIOFLUSH  2

/* baud rates */
#define B0      0
#define B50     50
#define B110    110
#define B300    300
#define B600    600
#define B1200   1200
#define B2400   2400
#define B4800   4800
#define B9600   9600
#define B19200  19200
#define B38400  38400
#define B57600  57600
#define B115200 115200

int     tcgetattr(int fd, struct termios *t);
int     tcsetattr(int fd, int action, const struct termios *t);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int     cfsetispeed(struct termios *t, speed_t speed);
int     cfsetospeed(struct termios *t, speed_t speed);
int     tcdrain(int fd);
int     tcflush(int fd, int queue_selector);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TERMIOS_H */
