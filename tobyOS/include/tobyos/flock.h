/* flock.h -- POSIX record locks + flock(2). See src/flock.c. */
#pragma once
#include <stdint.h>

struct file;
struct proc;

/* Linux x86-64 struct flock, as copied from/to userspace by the fcntl arm:
 *   short l_type; short l_whence; (4 pad) off_t l_start; off_t l_len;
 *   pid_t l_pid; (4 pad)  -- 32 bytes. */
struct lx_flock {
    int16_t l_type;      /* F_RDLCK=0 F_WRLCK=1 F_UNLCK=2 */
    int16_t l_whence;    /* SEEK_SET=0 SEEK_CUR=1 SEEK_END=2 */
    int32_t _pad0;
    int64_t l_start;
    int64_t l_len;       /* 0 = to EOF; negative = below l_start */
    int32_t l_pid;
    int32_t _pad1;
};

long fl_fcntl(struct file *f, struct proc *p, int cmd, struct lx_flock *fl);
long fl_flock(struct file *f, struct proc *p, int op);
void fl_release_close(struct file *f, struct proc *p);
void fl_release_ofd(void *ofd);
void fl_release_proc(struct proc *p);
