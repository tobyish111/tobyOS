/* selfhosttest -- M36E: compile /m36/m36_sample.c in-OS and run the ELF. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int s1 = system(
        "/bin/tobycc -o /data/self.elf /m36/m36_part2.c /m36/m36_sample.c");
    if (s1 < 0 || !WIFEXITED(s1) || WEXITSTATUS(s1) != 0) {
        puts("M36: FAIL tobycc");
        return 1;
    }
    int st = system("/data/self.elf");
    if (WIFEXITED(st) && WEXITSTATUS(st) == 42) {
        puts("M36: PASS self-host compile+run");
        return 0;
    }
    printf("M36: FAIL run status=%d\n", st);
    return 1;
}
