# bash 5's dynamic variables (S13): EPOCHSECONDS, EPOCHREALTIME, SRANDOM,
# BASHPID, BASH_ARGV0 -- all empty in tsh, and so were SECONDS and RANDOM.
# Formats and relations are compared, never values -- except RANDOM after
# a seed, whose sequence is bash's own generator.
echo "set: [${EPOCHSECONDS:+set}] [${EPOCHREALTIME:+set}] [${SRANDOM:+set}] [${BASHPID:+set}] [${BASH_ARGV0:+set}] [${SECONDS:+set}] [${RANDOM:+set}]"
case $EPOCHREALTIME in
  [0-9]*.[0-9][0-9][0-9][0-9][0-9][0-9]) echo "EPOCHREALTIME is secs.micros" ;;
  *) echo "EPOCHREALTIME looks like [$EPOCHREALTIME]" ;;
esac
s=$EPOCHSECONDS; r=${EPOCHREALTIME%.*}; d=$((r - s))
[ $d -ge 0 ] && [ $d -le 1 ] && echo "EPOCHSECONDS agrees with EPOCHREALTIME"
n=$(bash -c 'echo $EPOCHSECONDS'); d=$((EPOCHSECONDS - n)); [ ${d#-} -le 1 ] && echo "EPOCHSECONDS agrees with bash's"
[ "$BASHPID" = "$$" ] && echo "BASHPID is \$\$ at the top"
top=$$
{ [ "$$" = "$top" ] && echo "\$\$ in a pipeline stage is the shell's"; } | cat
{ [ "${$}" = "$top" ] && echo "\${\$} in a background job is the shell's"; } & wait
# A ( ) subshell runs IN-PROCESS in tsh (NEW-FINDINGS 16), so its BASHPID is
# $$; a pipeline stage and a background job are real processes.
{ [ "$BASHPID" != "$$" ] && echo "BASHPID differs in a pipeline stage"; } | cat
{ [ "$BASHPID" != "$$" ] && echo "BASHPID differs in a background job"; } & wait
a=$SRANDOM; b=$SRANDOM; [ "$a" != "$b" ] && echo "SRANDOM changes"
[ $SRANDOM -le 4294967295 ] && echo "SRANDOM fits 32 bits"
[ "$BASH_ARGV0" = "$0" ] && echo "BASH_ARGV0 is \$0"
BASH_ARGV0=renamed; echo "\$0 after assigning BASH_ARGV0: [$0]"
RANDOM=5; echo "seeded: $RANDOM $RANDOM $RANDOM"
RANDOM=4000000000; echo "seeded big: $RANDOM $RANDOM"
SECONDS=100; [ $SECONDS -ge 100 ] && [ $SECONDS -le 101 ] && echo "SECONDS counts on from 100"
EPOCHSECONDS=5; [ $EPOCHSECONDS -gt 5 ] && echo "assigning EPOCHSECONDS is ignored"
BASHPID=1; [ "$BASHPID" = "$$" ] && echo "assigning BASHPID is ignored"
unset RANDOM; RANDOM=7; echo "RANDOM after unset: $RANDOM $RANDOM"
unset SECONDS; echo "SECONDS after unset: [${SECONDS-gone}]"
unset EPOCHSECONDS; EPOCHSECONDS=5; echo "EPOCHSECONDS after unset+assign: $EPOCHSECONDS"
echo done
