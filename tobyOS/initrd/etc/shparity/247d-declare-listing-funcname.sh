# FUNCNAME outside a function is declared and invisible (`declare -p
# FUNCNAME` says `declare -a FUNCNAME`, `set` and -v do not see it), and
# `unset FUNCNAME` takes it away for good; the `declare -p`/`declare -a`
# listing is sorted by name, a declared-empty array included; the dynamic
# variables print as bash's do. tsh said "not found" and listed in
# creation order.
# (batch 12; probe p38 in C:\t\b12)
exec 2>&1
for v in RANDOM SRANDOM SECONDS EPOCHSECONDS EPOCHREALTIME BASHPID BASH_ARGV0 HISTCMD LINENO BASH_SUBSHELL BASH_COMMAND FUNCNAME BASH_LINENO BASH_SOURCE PIPESTATUS PPID UID EUID; do
  declare -p $v 2>&1 | sed -E 's/"[0-9.]+"/"N"/; s/^.*line [0-9]+: //'
done
