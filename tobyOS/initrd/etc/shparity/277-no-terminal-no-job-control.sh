# AN INTERACTIVE SHELL WITH NO TERMINAL HAS NO JOB CONTROL (batch 14), as
# bash's initialize_job_control finds: in a session with no controlling
# terminal (setsid) and fd 2 a pipe, /dev/tty is ENXIO and the pipe is "Not
# a tty", so it says "cannot set terminal process group (-1)" and "no job
# control in this shell", and $- has no m -- until a `set -m`. /bin/tsh
# looked at no terminal and kept job control on; the kernel had no
# controlling terminals (/dev/tty was the console for everyone) and no
# setsid. Also /dev/stdin, /dev/stdout, /dev/stderr, which did not exist.
# The shell's own name is left out of what it says (the prompt is set).
exec 2>&1
N() { sed 's/^[^:]*: /SH: /'; }
setsid -w "$BASH" -i -c 'echo "[$-]"' < /dev/null 2>&1 | N
setsid -w "$BASH" -m -c 'echo "[$-]"' < /dev/null 2>&1 | N
printf 'echo "[$-]"\nset -m; echo "rc=$? [$-]"\n' |
    PS1='$ ' setsid -w "$BASH" --norc -i 2>&1 | N
setsid -w cat /dev/tty 2>&1 | N
setsid -w "$BASH" -c 'exec 3</dev/tty' 2>&1 | N
echo via-stdin | cat /dev/stdin
echo via-redirect | "$BASH" -c 'cat < /dev/stdin'
echo to-stdout > /dev/stdout
echo end
