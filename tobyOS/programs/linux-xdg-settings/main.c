/* Minimal Linux ELF stub for chrome's default-browser xdg-settings probe.
 * Must be brandelf'd EI_OSABI=Linux so a Linux-personality chrome child can
 * exec it. Always exit_group(0). */
typedef long i64;
static inline i64 lx(i64 n, i64 a) {
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return ret;
}
#define SYS_exit_group 231
__attribute__((force_align_arg_pointer))
void _start(void) {
    lx(SYS_exit_group, 0);
    for (;;) { }
}
