/* su_sync.c -- sync; included by main.c. */

static int do_sync(void) {
#ifdef SU_HOST
    sync();
    long rc = 0;
#else
    long rc = sc0(ABI_SYS_SYNC);
#endif
    if (rc < 0) {
        fprintf(stderr, "sync: failed (rc=%ld)\n", rc);
        return 1;
    }
    return 0;
}
