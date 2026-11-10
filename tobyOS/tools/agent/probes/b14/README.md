# Batch 14 probes

Each `rN.sh` is one question put to the guest's bash 5.2.15 (musl) and to
/bin/tsh; the batch's commits cite them as "probe rN" or "logs/b14 rN".
Run one against a booted guest (QEMU on port 2222 here):

    ED_HOST=127.0.0.1 ED_PORT=2222 ED_USER=root \
        bash tools/agent/probe3.sh logs/b14/rN tools/agent/probes/b14/rN.sh

which leaves `logs/b14/rN.bash` and `logs/b14/rN.tsh` and says IDENTICAL
or DIFFERENT. A letter after the number in a log's name (r4c, r10c, r18b)
is a later run of the same probe on a later tree.

`k63.txt` is not a probe3 script: it is the list of lines the batch put to
the KERNEL shell (TCP 17777 in QEMU) for finding 63, one at a time --
logs/b14/k63-1.txt before the fix, k63-2 and k63-3 after. Its lasting form
is logs/kasyncprobe.sh, which compares the kernel shell with /bin/tsh and
runs in logs/kprobechain.sh. The kernel shell is one long-lived shell:
whatever a line leaves there (a variable, an integer attribute, a readonly)
is there for the next probe.

What differs by design: the shell's name in a prompt or diagnostic (r12),
and pids (r5 r7). bash's own races: in r5 bash's `wait -n` answers 127 or
0 from run to run (finding 73; tsh answers 0); in r16 the order of `set -x`
lines from concurrent pipeline elements varies (the files they write do
not). r15-r17 are checks that found nothing to fix: `wait` waits for every
element of a background pipeline, with and without job control (r15); a
background function that runs `cmd & wait` inside (r17).

Do not run `bash -i` without `setsid` in a probe -- it stops on SIGTTIN in
an SSH session -- nor `suspend -f`. The guest's sshd has ONE session slot:
free it from the kernel shell (TCP 17777: `ps`, `kill -9 PID`).

## Probe -> the commits that cite it

| probe | what | commits |
| --- | --- | --- |
| r1 | an and-or list ended by `&`; an async { } or function's EXIT trap | c10b332a e5b31796 1c36b95a |
| r2 | creation modes: open's mode less the umask, mkdir -m, tsh's redirections and history file | 25225d94 9897c234 e3374911 1f52c10c d63c08fb |
| r3 | rm -r of a directory with many entries | d63c08fb |
| r4 r5 | background pipelines under set -m: jobs -l, kill %N, wait | 16a8a49d |
| r6 r7 | background pipelines' pids, stdin and BASH_SUBSHELL; async stdin | 16a8a49d 2a9cdf58 959b5346 |
| r8 r9 r10 | which forked children run the EXIT trap they set | 1c36b95a |
| r11 | an async command without job control ignores SIGINT and SIGQUIT | 76d49dc6 |
| r12 | `bash -i` with no terminal; /dev/tty; /dev/stdin | 839eb41a bf36113d 959b5346 |
| r13 r14 | a forked child's EXIT trap writes inside its command's redirections | 08099195 |
| r15 r16 r17 | wait for a background pipeline; set -x ordering; `&` inside an async function | none (checks) |
| r18 | a closed descriptor stays closed; exec-in-place moves every source first | 060b5a59 |
| k63.txt | the kernel shell's `&` (finding 63) | a2e1c98e d31d81c9 5bbcab0d 417e983d |
