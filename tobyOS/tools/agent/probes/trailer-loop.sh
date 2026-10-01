# Fixture 71's race, counted: run a script whose last line is `echo
# trailing &` N times under a shell and read its output the moment the shell
# exits -- what shparity's harness does (waitpid on the shell, then read).
# Prints how many runs had the trailing line. Usage: SH=/bin/tsh N=100.
SH=${SH:-/bin/tsh}; N=${N:-100}
printf 'echo word_a &\nwait\necho word_b\necho middle\necho trailing &\n' > /tmp/trl.sh
got=0 i=0
while [ $i -lt $N ]; do
  $SH /tmp/trl.sh > /tmp/trl.out 2>&1
  case $(cat /tmp/trl.out) in *trailing*) got=$((got+1));; esac
  i=$((i+1))
done
echo "$SH: trailing present in $got/$N"
rm -f /tmp/trl.sh /tmp/trl.out
