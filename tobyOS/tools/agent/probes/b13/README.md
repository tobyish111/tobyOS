# Batch 13 probes

Each `qN.sh` is one question put to the guest's bash 5.2.15 (musl) and to
/bin/tsh; the batch's commits cite them as "probe qN" or "logs/b13 qN", in
their messages or in the code comments they add. Run one against a booted
guest (QEMU on port 2222 here):

    ED_HOST=127.0.0.1 ED_PORT=2222 ED_USER=root \
        bash tools/agent/probe3.sh logs/b13/qN tools/agent/probes/b13/qN.sh

which leaves `logs/b13/qN.bash` and `logs/b13/qN.tsh` and says IDENTICAL or
DIFFERENT. probe3.sh checks its upload by md5 (the first SSH sessions after
/bin/tsh is replaced sometimes drop their stdin); a NORESULT is retried.
`logs/b13/rN.*` are later re-runs of qN on the batch's tree; `fxN.*` are
the runs of `fxN.sh`, the drafts of shparity fixtures 258-272.

What differs by design: the shell's name in a diagnostic (`bash:` against
`tsh:`, q44b q45) and the width of a pid in a job line (q45). bash's own
races are races in tsh's runs too: where the notice of a job reaped by a
plain `wait` lands against the next line's output varies from run to run
in bash (q46 q52) -- q46v runs q46's first line eight times: bash put the
notice after "6" five times and before it three, tsh before it all eight
(logs/b13/q46v.*, on tobyOS-ea3e492.iso). q44.bash is empty: its `bash -i` cases stopped on
SIGTTIN in the SSH session, and q44b is q44 without them.

Do not run `suspend -f` or `bash -i` in a probe: the first stops the probe's
whole SSH session (bash stops the shell's process group), the second stops
on SIGTTIN. Free the slot from the kernel shell (TCP 17777: `ps`,
`kill -9 PID`).

## Probe -> the commits that cite it

| probe | commits | | probe | commits |
| --- | --- | --- | --- | --- |
| q0 | (the channel's smoke test) | | q30 | e59a2b9d |
| q1 q2 | d083cdd0 | | q31 | 8ad2cd88 9e1a59a5 |
| q3 | 4cd97e46 c439ec95 | | q32 | 9e1a59a5 |
| q4 | 8d7f775a | | q33 | ce2f1315 |
| q5 q7 | none: background function and builtin argv and side effects, IDENTICAL before any change | | q34 | e59a2b9d |
| q6 | df4c9f96 | | q35 | ce2f1315 e59a2b9d |
| q8 | 02858a4c | | q36 q37 | 1791f3c7 |
| q9 | 11e15b96 | | q38 | ce8bbd0b e465255d |
| q10 | 4cd97e46 c439ec95 2042bd33 | | q39 q40 | e465255d |
| q11 | c439ec95 7a2c3ee7 | | q41 | ce8bbd0b 19f2e2d9 |
| q12 | 2042bd33 | | q42 q43 q44 | 13b122c8 |
| q13 | 8d7f775a 5aba8a7b 4c1e6364 c1e27ae1 522d64ac c0a0b20a | | q44b q45 | job notices in scripts and -c strings (q44 less its -i cases; q45 under --posix): the shell's name and a pid's width differ, nothing else |
| q14 | 5aba8a7b c1e27ae1 8e14cde9 d11cebe6 | | q46-q49 q51 q52 | 13b122c8 (q46v: bash's race, above) |
| q15 | 8d7f775a 14a14ef5 c1e27ae1 | | q50 q53 | 322a1f62 85d20b8e |
| q16 | ce780931 582149b9 | | q54 | 1c998cfb 0d076120 50298ce7 e4b5e906 4f745471 |
| q17 | ce780931 29e3af42 ff198099 | | q55 | 3c159304 |
| q18 | 582149b9 | | q56 q57 | 1ca75f7e d53993c7 |
| q19 | 14a14ef5 | | q58 | d53993c7 |
| q20 | `times` and /proc/PID/stat's CPU fields, looked at by hand (no run kept) | | q59 | 0d076120 50298ce7 e4b5e906 4f745471 |
| q21 | c1e27ae1 | | fx258 | d083cdd0 |
| q22 q23 | faad9bed (q23 also c0a0b20a) | | fx259 | 8ad2cd88 |
| q24 | 522d64ac | | fx260 | 9e1a59a5 |
| q25 | c0a0b20a 299dc817 5df2d3bb | | fx261 | ce2f1315 |
| q26 | 299dc817 5df2d3bb 8e14cde9 | | fx262 fx263 | e59a2b9d |
| q27 q28 | 5df2d3bb 8e14cde9 | | fx264 | ce8bbd0b |
| q29 | ce8bbd0b | | fx265 | 1791f3c7 |
| | | | fx266 | e465255d |
| | | | fx267-fx269 | 13b122c8 |
| | | | fx270 | 322a1f62 |
| | | | fx271 | 3c159304 |
| | | | fx272 | d53993c7 |
