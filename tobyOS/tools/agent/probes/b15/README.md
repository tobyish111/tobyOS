# Batch 15 probes

Each `.sh` here is one question put to the guest's bash 5.2.15 (musl) and
to /bin/tsh unless it says otherwise; the batch's commits and findings cite
them as "logs/b15 NAME". Run a probe3 one against a booted guest (QEMU on
port 2222 here):

    ED_HOST=127.0.0.1 ED_PORT=2222 ED_USER=root \
        bash tools/agent/probe3.sh logs/b15/NAME tools/agent/probes/b15/NAME.sh

which leaves `logs/b15/NAME.bash` and `logs/b15/NAME.tsh` and says
IDENTICAL or DIFFERENT. A digit after a log's name (ttin1, kbusy-after2)
is a repeat run.

| Probe | Question | Findings, commits |
| --- | --- | --- |
| p1.sh | when each shell notices a job it has just killed; Oils 0944 0945 0488, each five times | 34 44 75, 10836839 |
| p2.sh | trapping or resetting a signal ignored in an async child, at entry, after `trap ''` | 72, 462b8f31 |
| p3.sh | signals ignored at entry and what children inherit (SigIgn bits) | 72 81, 462b8f31 |
| p4.sh, p4b.sh | the EXIT trap and fatal signals -- when, and for which | 76 79, 392d91a1 462b8f31 |
| p5.sh | a trapped signal against `wait`, a foreground child and `read`; SIGQUIT | 80 81, 462b8f31 |
| p6.sh | INT to a script waiting on a child; `( list ) &` as one process; `read -t` and signals | 83 84 85, 462b8f31 9b6d18ce 3e86f9e2 |
| p7.sh | a signal the parent CAUGHT, in its subshells; $? in the EXIT trap | 82, 462b8f31 |
| rt.sh | `read -t` on a pipe, a process substitution, a FIFO | 77 78 85 |
| fst.c | a static Linux binary: what fstat says of fd 0 and whether select sees it ready (built `gcc -static` in WSL's Ubuntu, gcc 15.2, glibc; uploaded and run under a pipe, a file, a terminal) | 77, 8e6be1e9 |
| lk.sh | links: `ls -l`, `test -L/-h/-e`, readlink, busybox stat, /dev/std*, /proc/self/fd | 68 70 87, b957ee09 4b9eb3b9 |
| lk3.sh | through a link to a directory: busybox `ls dir/`, `cat dl/f`, `cd` | 88, 4b9eb3b9 |
| cdl.sh | `cd` and `pwd` through a link, -L and -P, `set -P`, OLDPWD | eba4c207 |
| walk.sh | the native tree walkers on a tree with links out of it, to its ancestor and dangling | 87, 4b9eb3b9 |
| findbb.sh | the native find against busybox's on one tree | 73 94, e73ddbb6 |
| findcmp.sh | the native find against GNU find 4.10 (WSL): `findcmp.sh MYFIND` | 73, e73ddbb6 |
| diffr.sh | diff on two directories (DIFFBIN=...); compared with GNU diffutils 3.12 under WSL | 93, 4b9eb3b9 |
| stopw.sh | a process blocked in wait4, reading a pipe, `cat` on a pipe, sleep: stopped and continued -- compared with WSL's Linux 6.6 | 90, e7d5c644 |
| ttin.sh | NOT probe3: an `ssh -tt` login session per shell, `cat &` (SIGTTIN), `kill -9` of the stopped job, a ( ) job reading then `fg` | 89 (and 34's kill -9 note), 1bcc1150 57bf1a6f |
| waitn.sh | bash's own `wait -n` race, minimal forms, 20 runs each (none) | 74 |
| waitn2.sh | b14/r5 whole, 10 runs (bash: 0 six times, 127 four) -- needs r5.sh at /tmp/pr | 74 |

Kernel-shell probes of the batch live in logs/: kbusyprobe.py (finding 67,
in kprobechain) and kbigvalprobe.py.

What differs by design: pipe numbers (lk), the script's own name (lk), and
WSL's bash 5.3 on Oils 0463 (a version difference, finding 74).
