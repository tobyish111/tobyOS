echo hello | { read i; echo $i;} | { read i; echo $i;} | cat
echo hello | while read i; do echo -=$i=- | sed s/=/@/g ; done | cat
