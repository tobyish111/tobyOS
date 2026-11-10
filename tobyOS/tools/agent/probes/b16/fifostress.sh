# fifostress.sh N: fixture 297's FIFO steps f6-f10, N times in a fresh
# $SH each, every run under a 10 s timeout -- shparity hung once in f9
# (bash's `while read ...; done < p1` never ended). Counts the endings.
N=${1:-50}
cat > /tmp/fst.sh <<'EOF'
d=/tmp/fst.$$; rm -rf $d; mkdir -p $d; cd $d || exit 9
mkfifo p1
( echo hello > p1 ) & cat p1 > /dev/null; wait
cat p1 > /dev/null & echo world > p1; wait
exec 8<> p1; echo first >&8; echo second >&8; read a <&8; read b <&8; exec 8<&-
( for i in 1 2 3; do echo "line $i"; done > p1 ) & n=0; while read x; do n=$((n+1)); done < p1; wait
( exec 3> p1; echo one >&3; exec 3>&- ) & read l < p1; wait
cd /; rm -rf $d
echo "ok $a $b $n $l"
EOF
i=0; while [ $i -lt $N ]; do i=$((i+1))
  timeout 10 $SH /tmp/fst.sh 2>&1; echo "rc=$?"
done | sort | uniq -c
