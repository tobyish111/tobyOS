# BASH_SUBSHELL in ( ), $( ), pipelines and & ; exporting long names and
# unsetting an exported associative array.
# (batch 12; probe p39 in C:\t\b12)
exec 2>&1
echo "1 $BASH_SUBSHELL $(echo $BASH_SUBSHELL) $( (echo $BASH_SUBSHELL) )"
( echo "2 $BASH_SUBSHELL $(echo $BASH_SUBSHELL)" )
echo "3 $BASH_SUBSHELL" | cat
{ echo "4 $BASH_SUBSHELL"; } | cat
x=$(echo $BASH_SUBSHELL); echo "5 $x"
f() { echo "6 $BASH_SUBSHELL"; }; f; (f); echo "7 $(f)"
echo "8 ${BASH_SUBSHELL}" &
wait
PPID=3; echo "9 s=$?"
UID=4; echo "10 s=$?"
declare -p PPID UID EUID | sed -E 's/"[0-9]+"/"N"/'
echo "11 $SHLVL"
bash_v=${BASH_VERSION%%.*}; echo "12 $bash_v ${BASH_VERSINFO[0]} ${#BASH_VERSINFO[@]}"
BASH_VERSINFO=x; echo "13 s=$?"
x=$(echo $BASH_SUBSHELL | cat); echo "14 $x"
y=$({ echo $BASH_SUBSHELL; } | cat); echo "15 $y"
{ echo "16 $BASH_SUBSHELL" | cat; } | cat
( echo "17 $BASH_SUBSHELL" & wait )
