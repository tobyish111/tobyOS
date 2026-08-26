# POSIX 2.9.4.4/2.9.4.5 -- while and until, plus `break N` / `continue N`,
# which unwind more than one enclosing loop and are the part shells usually
# implement as a single boolean flag and get wrong at depth 2.
i=0
while [ $i -lt 3 ]; do i=$((i+1)); echo "1:$i"; done

j=0
until [ $j -ge 3 ]; do j=$((j+1)); echo "2:$j"; done

k=0
while [ $k -lt 10 ]; do
    k=$((k+1))
    if [ $k -eq 2 ]; then continue; fi
    if [ $k -eq 4 ]; then break; fi
    echo "3:$k"
done
echo "4=$k"

for a in 1 2 3; do
    for b in x y z; do
        if [ "$b" = y ]; then continue 2; fi
        echo "5:$a$b"
    done
    echo "6:not-reached-$a"
done

for a in 1 2 3; do
    for b in x y; do
        if [ "$a" = 2 ]; then break 2; fi
        echo "7:$a$b"
    done
done

m=0
while true; do
    m=$((m+1))
    [ $m -ge 3 ] && break
done
echo "8=$m"

while false; do echo "9:never"; done
echo "10=$?"
until true; do echo "11:never"; done
echo "12=$?"

n=0
while [ $n -lt 2 ]; do n=$((n+1)); echo "13:$n"; done > wout.txt
while read l; do echo "14:$l"; done < wout.txt
echo end
