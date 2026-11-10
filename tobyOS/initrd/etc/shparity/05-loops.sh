# for / while / until plus break and continue. Nested loops are included
# because a shell that tracks loop depth with a single flag rather than a
# counter passes the flat cases and fails here.
for w in a b c; do echo "for:$w"; done
i=0
while [ $i -lt 3 ]; do
    echo "while:$i"
    i=$((i + 1))
done
j=0
until [ $j -ge 2 ]; do
    echo "until:$j"
    j=$((j + 1))
done
for k in 1 2 3 4 5; do
    if [ $k -eq 2 ]; then continue; fi
    if [ $k -eq 4 ]; then break; fi
    echo "ctl:$k"
done
for a in x y; do
    for b in 1 2; do
        echo "nest:$a$b"
    done
done
for e in; do echo "never:$e"; done
echo loops-done
