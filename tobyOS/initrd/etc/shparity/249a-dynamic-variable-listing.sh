# In the `declare -p` listing a dynamic variable (RANDOM, SECONDS, LINENO,
# BASHPID, COMP_WORDBREAKS, ...) shows the value it was last read as, or
# none if it never was; `set` lists it only once read; -v is true; TERM
# defaults to dumb and OPTIND is an integer. tsh left them all out.
# (batch 12; probe p87 in C:\t\b12)
exec 2>&1
declare -p | grep -E ' (RANDOM|SECONDS|LINENO|BASHPID|EPOCHSECONDS|BASH_COMMAND|SRANDOM|HISTCMD|BASH_SUBSHELL|BASH_ARGV0|EPOCHREALTIME)(=|$)' | sed -E 's/="[0-9.]+"/="N"/'
x=$RANDOM$SECONDS$LINENO$BASHPID$EPOCHSECONDS$SRANDOM$HISTCMD$BASH_SUBSHELL$BASH_ARGV0$EPOCHREALTIME
echo "== after read"
declare -p | grep -E ' (RANDOM|SECONDS|LINENO|BASHPID|EPOCHSECONDS|BASH_COMMAND|SRANDOM|HISTCMD|BASH_SUBSHELL|BASH_ARGV0|EPOCHREALTIME)(=|$)' | sed -E 's/="[^"]*"/="V"/'
echo "== set after read"
set | grep -E '^(RANDOM|SECONDS|LINENO|BASHPID|EPOCHSECONDS|BASH_COMMAND|SRANDOM|HISTCMD|BASH_SUBSHELL|BASH_ARGV0|EPOCHREALTIME)=' | sed -E 's/=.*//'
echo "== same value?"
a=$RANDOM; declare -p | grep ' RANDOM=' | sed -E "s/$a/SAME/"
echo "== -v"
[[ -v RANDOM ]]; echo "v=$?"
