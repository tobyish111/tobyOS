/* realcc proof source: compiled IN-VM by a REAL, unmodified TinyCC and then
 * run on tobyOS. Freestanding on purpose -- no libc, no headers, no crt -- so
 * the compile needs nothing but the compiler itself (tcc -nostdlib -static):
 * it defines its own _start and talks to the kernel through raw x86-64 Linux
 * syscalls. A successful run proves tcc turned this C into machine code, emitted
 * a valid ELF, and tobyOS loaded + executed it. */

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

void _start(void) {
    const char msg[] = "CC-OK built-by-real-tcc\n";
    sys3(1, 1, (long)msg, sizeof(msg) - 1);   /* write(1, msg, len) */
    sys3(60, 42, 0, 0);                        /* exit(42)           */
    __builtin_unreachable();
}
