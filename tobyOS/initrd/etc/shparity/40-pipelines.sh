# POSIX 2.9.2 -- pipelines.
# The part tsh is known to fail: a pipeline stage MAY be a compound command.
# `cmd | while read l; do ...; done` is one of the most common shapes in real
# scripts, and a shell that only pipes into simple commands tries to execute
# `while` as a program.
printf 'a\nb\nc\n' > src.txt

while read l; do echo "plain:$l"; done < src.txt

echo one | while read l; do echo "pipe-while:$l"; done

printf 'x\ny\n' | while read l; do echo "multi:$l"; done

echo z | { read l; echo "pipe-group:$l"; }

echo q | if read l; then echo "pipe-if:$l"; fi

printf '1\n2\n' | while read n; do
    if [ "$n" = 1 ]; then echo "one"; else echo "other:$n"; fi
done

n=0
printf 'p\nq\nr\n' | while read l; do n=$((n+1)); echo "count:$n"; done

echo start | cat
echo status-last=$(true | false; echo $?)
echo status-last2=$(false | true; echo $?)
! false | true; echo "negated=$?"
echo end
