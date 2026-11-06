# The shell's descriptors are its process's, at their own numbers, as
# bash's are: /proc/$$/fd lists a slot at its number, a {v} at 10, a coproc
# at 63 and 60, the script on 255, bash's saved copies of a redirected 0-2
# from 10 up -- and not an external command's own redirections, which the
# child opens. tsh's were whatever open() handed out (finding 618).
exec 2>&1
L=/tmp/shp-fd618.$$
l() { ls /proc/$$/fd > $L; tr '\n' ' ' < $L; echo; }
l
exec 5>/dev/null; exec {v}>/dev/null; l; echo "v=$v"
exec 6>&5; l
exec 5>&- 6>&-; eval "exec $v>&-"; l
{ l; } 7>/dev/null 9</dev/null
g() { l; }; g 2>/dev/null </dev/null
coproc cat; l; echo "${COPROC[@]}"; eval "exec ${COPROC[1]}>&-"; wait
rm -f $L
