/* termios.h -- libtoby's terminal I/O control. */

#ifndef LIBTOBY_TERMIOS_H
#define LIBTOBY_TERMIOS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 20

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

/* c_lflag bits */
#define ECHO    0x0001
#define ECHOE   0x0002
#define ECHOK   0x0004
#define ECHONL  0x0008
#define ICANON  0x0010
#define ISIG    0x0020
#define IEXTEN  0x0040
#define TOSTOP  0x0080
#define NOFLSH  0x0100

/* c_iflag bits */
#define ICRNL   0x0001
#define INLCR   0x0002
#define IGNCR   0x0004
#define IXON    0x0008
#define IXOFF   0x0010
#define IGNBRK  0x0020
#define BRKINT  0x0040
#define ISTRIP  0x0080
#define INPCK   0x0100
#define IGNPAR  0x0200
#define PARMRK  0x0400
#define IXANY   0x0800

/* c_oflag bits */
#define OPOST   0x0001
#define ONLCR   0x0002

/* c_cflag bits */
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

/* c_cc indices */
#define VEOF    0
#define VEOL    1
#define VERASE  2
#define VINTR   3
#define VKILL   4
#define VMIN    5
#define VQUIT   6
#define VSTART  7
#define VSTOP   8
#define VSUSP   9
#define VTIME  10

/* tcsetattr actions */
#define TCSANOW    0
#define TCSADRAIN  1
#define TCSAFLUSH  2

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
