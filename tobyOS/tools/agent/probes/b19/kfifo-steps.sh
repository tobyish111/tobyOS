# kfifo-steps.sh: tools/agent/probes/b19/kfifo.py's steps as a script, for
# the guest's bash (and /bin/tsh) -- the values kfifo.py wants of the kernel
# shell. k8 (an SSH writer ending a wait) is kfifo.py's alone.
exec 2>&1
rm -f /tmp/kf /tmp/kf.out; mkfifo /tmp/kf; echo k1-done mk=$?
test -p /tmp/kf; echo "k2-done p=$?"
echo hello > /tmp/kf & echo k3a-done
read v < /tmp/kf; echo "k3-done st=$? v=$v"
wait
/bin/cat /tmp/kf > /tmp/kf.out & echo k4a-done
echo world > /tmp/kf; echo "k4-done st=$?"
sleep 1; echo "k4b-done out=$(cat /tmp/kf.out)"
wait
exec 3<>/tmp/kf; echo both >&3; read w <&3; exec 3<&-; echo "k5-done w=$w"
printf 'a\nbb\n' > /tmp/kf & echo k6a-done
n=$(wc -c < /tmp/kf); echo "k6-done n=$((n))"
wait
printf 'x\ny\nz\n' > /tmp/kf & echo k7a-done
c=0; while read l; do c=$((c+1)); done < /tmp/kf; echo "k7-done c=$c"
wait
rm -f /tmp/kf /tmp/kf.out; echo "k9-done st=$?"
