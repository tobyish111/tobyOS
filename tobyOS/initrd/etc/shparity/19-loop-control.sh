# GAP 5: break and continue, in all three loop forms and nested.
# The failure this pins was subtle: `if ...; then continue; fi` worked, but
# the commands AFTER that if on the same line were dropped, so the loop body
# silently lost everything past its first compound.
for i in 1 2 3 4 5; do
    if [ $i -eq 2 ]; then continue; fi
    if [ $i -eq 4 ]; then break; fi
    echo "for:$i"
done
echo "after-for"

n=0
while [ $n -lt 6 ]; do
    n=$((n + 1))
    if [ $n -eq 2 ]; then continue; fi
    if [ $n -eq 4 ]; then break; fi
    echo "while:$n"
done
echo "n=$n"

u=0
until [ $u -ge 6 ]; do
    u=$((u + 1))
    if [ $u -eq 3 ]; then break; fi
    echo "until:$u"
done

for a in 1 2; do
    for b in x y; do
        if [ "$b" = "y" ]; then continue; fi
        echo "nest:$a$b"
    done
done

for a in 1 2 3; do
    for b in x y; do
        if [ "$b" = "y" ]; then break; fi
        echo "inner:$a$b"
    done
    if [ "$a" = "2" ]; then break; fi
done
echo loopctl-done
