#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
/* Can a process reap its SIBLING?  On Linux: never (ECHILD).  The parent
 * forks p (sleeps), then q; kills p; q tries waitpid(-1), waitpid(p),
 * waitpid(0) with WNOHANG; then the parent waits for p itself. */
static void try(const char *what, pid_t want, pid_t p) {
    int st = 0;
    pid_t r = waitpid(want, &st, WNOHANG);
    if (r < 0) printf("child waitpid(%s) -> %s\n", what, errno == ECHILD ? "ECHILD" : strerror(errno));
    else if (r == p) printf("child waitpid(%s) -> REAPED THE SIBLING\n", what);
    else printf("child waitpid(%s) -> %d\n", what, (int)r);
    fflush(stdout);
}
int main(void) {
    pid_t p = fork();
    if (p == 0) { sleep(5); _exit(7); }
    pid_t q = fork();
    if (q == 0) {
        usleep(500000);
        try("-1", -1, p); try("p", p, p); try("0", 0, p);
        _exit(0);
    }
    usleep(200000);
    kill(p, SIGKILL);
    int st = 0;
    waitpid(q, &st, 0);
    pid_t r = waitpid(p, &st, 0);
    if (r == p) printf("parent waitpid(p) -> own child, signal %d\n", WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    else printf("parent waitpid(p) -> %s\n", errno == ECHILD ? "ECHILD (someone else reaped it)" : strerror(errno));
    return 0;
}
