/* su_seq.c -- seq; included by main.c. */

static int do_seq(int argc, char **argv) {
    long first = 1, incr = 1, last = 0;
    const char *sep = "\n";
    const char *nums[3];
    int nn = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) { sep = argv[++i]; continue; }
        if (!strncmp(argv[i], "-s", 2) && argv[i][2]) { sep = argv[i] + 2; continue; }
        if (!strcmp(argv[i], "-w")) continue;      /* equal width: accepted */
        if (nn < 3) nums[nn++] = argv[i];
    }
    if (nn == 0) { fprintf(stderr, "usage: seq [first [incr]] last\n"); return 1; }
    if (nn == 1) { last = atol(nums[0]); }
    else if (nn == 2) { first = atol(nums[0]); last = atol(nums[1]); }
    else { first = atol(nums[0]); incr = atol(nums[1]); last = atol(nums[2]); }
    if (incr == 0) { fprintf(stderr, "seq: increment must not be zero\n"); return 1; }
    int printed = 0;
    for (long v = first; incr > 0 ? v <= last : v >= last; v += incr) {
        if (printed) fputs(sep, stdout);
        printf("%ld", v);
        printed = 1;
    }
    if (printed) putchar('\n');
    return 0;
}
