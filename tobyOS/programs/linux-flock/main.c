/* linux-flock -- POSIX record locks + flock(2) acceptance test (2026-08-22).
 *
 * Locks only mean anything BETWEEN processes, so every real assertion here
 * is made from a forked child against a lock the parent holds, synchronised
 * with pipe handshakes (no sleeps -- the linux-pidns pattern). And every
 * refusal asserts its ERRNO: "the lock failed" is also what EBADF looks
 * like, and a test that accepts any failure passes on a kernel with no
 * locking at all.
 *
 * Before src/flock.c, F_SETLK/F_SETLKW/F_GETLK and flock() were all
 * `return 0`: two processes could both "hold" an exclusive lock on the
 * same bytes. bit1 is that exact scenario.
 *
 *   bit0  F_SETLK(WRLCK) succeeds; F_GETLK from the SAME process says
 *         F_UNLCK (a process never conflicts with itself)
 *   bit1  a child's F_SETLK on the parent's locked range is EAGAIN, and
 *         its F_GETLK names the parent's pid and F_WRLCK
 *   bit2  after the parent unlocks, the child acquires the same range
 *   bit3  range surgery: parent locks [0,10) then unlocks [4,6); child
 *         gets [4,6) but EAGAIN on [0,2) -- the split really happened
 *   bit4  flock: parent LOCK_EX; child LOCK_EX|LOCK_NB is EWOULDBLOCK;
 *         after parent LOCK_UN the child acquires
 *   bit5  the POSIX close wart: locks placed via fd A drop when the same
 *         process closes fd B to the same file -- child can then lock
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>

#define TFILE "/data/flock.t"

/* Pipe handshake: parent and child each block on a 1-byte read until the
 * peer says go. */
static int p2c[2], c2p[2];
static void tell_child(void)  { (void)!write(p2c[1], "g", 1); }
static void tell_parent(void) { (void)!write(c2p[1], "g", 1); }
static void wait_parent(void) { char b; (void)!read(p2c[0], &b, 1); }
static void wait_child(void)  { char b; (void)!read(c2p[0], &b, 1); }

static int lk(int fd, int cmd, short type, off_t start, off_t len,
              struct flock *out) {
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = type; fl.l_whence = SEEK_SET;
    fl.l_start = start; fl.l_len = len;
    int rc = fcntl(fd, cmd, &fl);
    if (out) *out = fl;
    return rc;
}

/* The child: mirrors the parent's script step by step. Its result bits ride
 * its exit status; the parent folds them in. */
static int child_main(void) {
    int bits = 0;
    int fd = open(TFILE, O_RDWR);
    if (fd < 0) return 0;

    /* bit1: parent holds WR [0,10). */
    wait_parent();
    errno = 0;
    struct flock g;
    int r = lk(fd, F_SETLK, F_WRLCK, 0, 10, 0);
    int r_eagain = (r == -1 && (errno == EAGAIN || errno == EACCES));
    lk(fd, F_GETLK, F_WRLCK, 0, 10, &g);
    fprintf(stderr, "flock-child: SETLK rc=%d errno=%d GETLK type=%d pid=%d "
                    "(ppid=%d)\n", r, errno, g.l_type, (int)g.l_pid,
            (int)getppid());
    if (r_eagain && g.l_type == F_WRLCK && g.l_pid == getppid()) bits |= 1;
    tell_parent();

    /* bit2: parent unlocked; same range must now be ours. */
    wait_parent();
    if (lk(fd, F_SETLK, F_WRLCK, 0, 10, 0) == 0 &&
        lk(fd, F_SETLK, F_UNLCK, 0, 10, 0) == 0) bits |= 2;
    tell_parent();

    /* bit3: parent holds [0,10) minus the unlocked hole [4,6). */
    wait_parent();
    int hole_ok = (lk(fd, F_SETLK, F_WRLCK, 4, 2, 0) == 0);
    errno = 0;
    int low = lk(fd, F_SETLK, F_WRLCK, 0, 2, 0);
    int low_blocked = (low == -1 && (errno == EAGAIN || errno == EACCES));
    fprintf(stderr, "flock-child: hole=%d low_blocked=%d\n",
            hole_ok, low_blocked);
    if (hole_ok && low_blocked) bits |= 4;
    lk(fd, F_SETLK, F_UNLCK, 4, 2, 0);
    tell_parent();

    /* bit4: flock. Parent holds LOCK_EX. */
    wait_parent();
    errno = 0;
    int fr = flock(fd, LOCK_EX | LOCK_NB);
    int fr_blocked = (fr == -1 && errno == EWOULDBLOCK);
    tell_parent();
    wait_parent();                       /* parent released */
    int fr2 = flock(fd, LOCK_EX | LOCK_NB);
    fprintf(stderr, "flock-child: flock nb rc=%d errno=%d then rc=%d\n",
            fr, errno, fr2);
    if (fr_blocked && fr2 == 0) bits |= 8;
    if (fr2 == 0) flock(fd, LOCK_UN);
    tell_parent();

    /* bit5: parent locked via fdA then closed fdB -- wart says the lock is
     * GONE and this succeeds. */
    wait_parent();
    if (lk(fd, F_SETLK, F_WRLCK, 20, 5, 0) == 0) bits |= 16;
    tell_parent();

    close(fd);
    return bits;
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    int fd = open(TFILE, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { printf("flock: cannot create %s errno=%d\n", TFILE, errno); return 0; }
    (void)!write(fd, "0123456789012345678901234567890", 31);

    if (pipe(p2c) != 0 || pipe(c2p) != 0) return 0;

    /* bit0 needs no peer. */
    if (lk(fd, F_SETLK, F_WRLCK, 0, 10, 0) == 0) {
        struct flock g;
        lk(fd, F_GETLK, F_WRLCK, 0, 10, &g);
        printf("flock: own GETLK over own lock -> type=%d (want F_UNLCK=%d)\n",
               g.l_type, F_UNLCK);
        if (g.l_type == F_UNLCK) bits |= 1;
    }

    pid_t pid = fork();
    if (pid == 0) _exit(child_main());
    if (pid < 0) return bits;

    /* bit1: we still hold WR [0,10). */
    tell_child(); wait_child();

    /* bit2: release, let the child take it. */
    lk(fd, F_SETLK, F_UNLCK, 0, 10, 0);
    tell_child(); wait_child();

    /* bit3: lock [0,10), punch out [4,6). */
    lk(fd, F_SETLK, F_WRLCK, 0, 10, 0);
    lk(fd, F_SETLK, F_UNLCK, 4, 2, 0);
    tell_child(); wait_child();
    lk(fd, F_SETLK, F_UNLCK, 0, 10, 0);

    /* bit4: flock round. */
    flock(fd, LOCK_EX);
    tell_child(); wait_child();
    flock(fd, LOCK_UN);
    tell_child(); wait_child();

    /* bit5: the close wart. Lock [20,25) via fd, then open+close a SECOND
     * fd to the same file -- POSIX drops our lock. */
    lk(fd, F_SETLK, F_WRLCK, 20, 5, 0);
    int fd2 = open(TFILE, O_RDONLY);
    if (fd2 >= 0) close(fd2);
    tell_child(); wait_child();

    int st = 0;
    if (waitpid(pid, &st, 0) == pid && WIFEXITED(st))
        bits |= WEXITSTATUS(st) << 1;    /* child bits 0..4 -> bits 1..5 */

    close(fd);
    unlink(TFILE);
    printf("LXFLOCK: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
