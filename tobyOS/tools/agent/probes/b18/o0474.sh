# o0474.sh: Oils 0474 ten times under each shell, each in a fresh directory
# as the harness runs it; each distinct output and how often.
for sh in bash /bin/tsh; do
  for i in 1 2 3 4 5 6 7 8 9 10; do
    d=/tmp/o474.$$.$i; mkdir -p $d; cd $d
    $sh /etc/oilspec/0474.sh 2>&1 | tr '\n' '|'; echo " exit=$?"
    cd /; rm -rf $d
  done | sort | uniq -c | sed "s#^#$sh: #"
done
