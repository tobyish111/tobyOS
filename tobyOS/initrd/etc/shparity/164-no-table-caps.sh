# Jobs, the directory stack, the hash table, completion specs and history
# have no fixed size (S6). tsh held 8 jobs (and its `jobs` reached a
# kernel-only call in a script), 33 directories, 64 hashed commands, 32
# completion specs and no settable history at all.
for i in $(seq 12); do sleep 3 & done
echo "jobs $(jobs | wc -l) pids $(jobs -p | wc -l)"
kill $(jobs -p); wait
sleep 3 & sleep 3 & jobs | cut -c1-5; kill $(jobs -p); wait
cd /tmp; for i in $(seq 40); do pushd /tmp >/dev/null; done
echo "dirs $(dirs -p | wc -l)"
dirs -c; pushd -n /usr >/dev/null; echo "pushd -n stays: $PWD $(dirs -p | wc -l)"
popd -n >/dev/null; echo "popd -n: $(dirs -p | wc -l)"
pushd -x 2>/dev/null; echo "pushd -x: $?"; popd x 2>/dev/null; echo "popd x: $?"
n=0; for c in $(ls /bin | head -80); do hash -p /bin/$c $c && n=$((n+1)); done
echo "hashed $n listed $(hash | tail -n +2 | wc -l)"
for i in $(seq 40); do complete -W x cmd$i; done
echo "complete $(complete -p | grep -c 'cmd[0-9]')"
HISTSIZE=1000; set -o history; history -c
for i in $(seq 300); do history -s "cmd $i"; done
echo "history $(history | wc -l)"
history -d 1; echo "after -d $(history | wc -l)"
set +o history; history -s "stored anyway"; echo "after +o history $(history | wc -l)"
echo "[$HISTSIZE]"
# The rules, one by one, as the guest's bash 5.2 has them.
set -o history; history -c
history -s a1; history -s a2; history -s a3; history -s a4; history -s a5
history -d 2; history -d -1; history -d 0-1; echo "d rc=$?"; history
history -d 9-9 2>/dev/null; echo "out of range: $?"; history -d 3-2; echo "backwards: $?"
history abc 2>/dev/null; echo "not a number: $?"
f=/tmp/fx164.$$; HISTFILE=$f; history -w; echo "w lines $(wc -l < $f)"
history -c; history -r; echo "r $(history | wc -l)"
printf 'n1\nn2\n' >> $f; history -n; history | tail -3
history -a; echo "a lines $(wc -l < $f)"
history -p x y; history | tail -1
HISTSIZE=3; history; unset HISTSIZE
printf 'c%s\n' 1 2 3 4 5 6 > $f; HISTFILESIZE=2; cat $f; rm -f $f
PS1='[\!]'; echo "prompt ${PS1@P}"
set -o | grep -w history; set +o history; set -o | grep -w history
echo done
