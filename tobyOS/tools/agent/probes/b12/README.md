# Batch 12 probes

Each `pN.sh` (and `fn1.sh`, `cpk*.sh`) is one question put to the guest's
bash 5.2.15 and to /bin/tsh: the batch's commits cite them as "probe pN".
Run one against a booted guest (QEMU on port 2222 here; the EliteDesk by
default):

    ED_HOST=127.0.0.1 ED_PORT=2222 ED_USER=root \
        bash tools/agent/probe3.sh logs/b12/pN tools/agent/probes/b12/pN.sh

which leaves `logs/b12/pN.bash` and `logs/b12/pN.tsh` and says IDENTICAL or
DIFFERENT. A probe prints values that are the same in both shells -- pids,
times and the shell's own name are left out or normalised where they could
show (p49 p66 p67 p74 p80 print a pid in a job report and differ there only;
p31 p77 print BASH/MACHTYPE and the environment order, which differ by
design).

The deterministic ones became shparity fixtures 220-257
(initrd/etc/shparity), one fixture per probe.

## Late in the batch: job notification, coprocess cleanup, redirections

p81 and p91 were made race-free: each coprocess their line reads from now
waits for a line from the shell before it ends (bash disposes of one that
has ended before it finishes starting it -- a race in bash itself).

p92-p106, p111 and p112 measure WHERE bash reports a job's death and cleans
up its table -- after a foreground wait, at each line its reader takes
(including a $( )'s text, an eval string, a sourced file, a trap action),
at a job's start and in loops -- and which line it names; p107-p110 the
line of an assignment-first command, a multi-line <( ), and the word after
`<&` / `>&`. Several kill a job and wait it out with a loop of builtins
(`e=$((SECONDS+2)); while ((SECONDS<e)); do :; done`), so bash has seen it
die; a report carries a pid, so compare them with the pids replaced:

    sed 's/: *[0-9][0-9]* Killed/: PID Killed/' logs/b12/n3-p99.bash

Their outputs from bash 5.2.15 in WSL are in logs/b12/wsl (*.wsl1, *.wsl2:
two runs). p93, p94, p95 and p98 show races in bash itself: WSL's bash and
the guest's put some reports in different places.

t945/ (o945 is Oils 0945; t2-t5 variants) measure how soon bash sees a job
the shell has just killed: on the guest, not at the next line read, but a
line or two later; in WSL, later still.

p113 globs words that only look like assignments (finding 54); p118
puts `case` and `esac` where they are only words.

wsib.c is a static Linux ELF (gcc -static in WSL): can a process reap its
SIBLING with waitpid(pid)? Linux: ECHILD. This kernel said yes until this
batch (logs/b12/wsib-before.txt, wsib-after.txt).
