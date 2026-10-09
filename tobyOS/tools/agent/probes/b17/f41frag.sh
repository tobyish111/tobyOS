# f41frag.sh: 200 $( ) before and after a 20000-element array is freed -- the
# slowdown after is libtoby malloc's one best-fit free list (finding 107).
s=$(cut -d" " -f1 /proc/uptime)
j=0; while [ $j -lt 200 ]; do x=$(echo $j); j=$((j+1)); done
e=$(cut -d" " -f1 /proc/uptime)
echo "fresh: $(echo "$e $s" | awk "{printf \"%.2f\", \$1-\$2}")s"
b=(); i=0; while [ $i -lt 20000 ]; do b[i]=element-$i; i=$((i+1)); done; unset b
s=$(cut -d" " -f1 /proc/uptime)
j=0; while [ $j -lt 200 ]; do x=$(echo $j); j=$((j+1)); done
e=$(cut -d" " -f1 /proc/uptime)
echo "after unset: $(echo "$e $s" | awk "{printf \"%.2f\", \$1-\$2}")s"
