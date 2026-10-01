# Oils 0488 under bash, N times, counting the runs whose output got "Hangup"
# -- once with the case's bash in its parent's process group, once leading a
# group of its own, as the Oils harness has run it since its process-group
# change. Run THIS with bash: `set -m` in bash puts each foreground job in a
# group of its own. Usage: N=40 bash o0488-loop.sh
N=${N:-40}
mkdir -p /tmp/o488 && cd /tmp/o488 || exit 1
cp /etc/oilspec/0488.sh c.sh
count() {
  h=0 i=0
  while [ $i -lt $N ]; do
    rm -f err.txt out.txt
    bash c.sh > out.txt 2>/dev/null
    grep -q Hangup out.txt && h=$((h+1))
    i=$((i+1))
  done
}
# Called directly, not in $( ): a subshell does no job control.
count; echo "same group: Hangup in $h/$N"
set -m
count; echo "own group:  Hangup in $h/$N"
set +m
cd / && rm -rf /tmp/o488
