a=(); i=0; while [ $i -lt 20000 ]; do a[i]=element-$i; i=$((i+1)); done
s=$(cut -d" " -f1 /proc/uptime)
j=0; while [ $j -lt 200 ]; do x=$(echo $j); j=$((j+1)); done
e=$(cut -d" " -f1 /proc/uptime)
echo "200 cmdsubs with a 20000-element array: $(echo "$e $s" | awk '{printf "%.2f", $1-$2}')s"
unset a
s=$(cut -d" " -f1 /proc/uptime)
j=0; while [ $j -lt 200 ]; do x=$(echo $j); j=$((j+1)); done
e=$(cut -d" " -f1 /proc/uptime)
echo "200 cmdsubs with no array: $(echo "$e $s" | awk '{printf "%.2f", $1-$2}')s"
