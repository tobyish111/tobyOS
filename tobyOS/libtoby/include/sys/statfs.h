/* sys/statfs.h -- statfs(2) and fstatfs(2) (finding 215: libtoby had
 * neither, and df and stat -f carried their own). The struct is Linux's
 * x86-64 one: the kernel's ABI_SYS_LXSTATFS answers in it, as the Linux
 * personality's statfs does. */
#ifndef LIBTOBY_SYS_STATFS_H
#define LIBTOBY_SYS_STATFS_H

typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;
typedef struct { int __val[2]; } fsid_t;

struct statfs {
    long       f_type;      /* the filesystem's magic number */
    long       f_bsize;     /* the transfer block size */
    fsblkcnt_t f_blocks;    /* blocks in all, of f_frsize bytes */
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;    /* free to an unprivileged user */
    fsfilcnt_t f_files;     /* inodes */
    fsfilcnt_t f_ffree;
    fsid_t     f_fsid;
    long       f_namelen;   /* the longest name */
    long       f_frsize;    /* the fragment size */
    long       f_flags;     /* the mount's ST_* flags */
    long       f_spare[4];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif
