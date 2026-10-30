# test -v NAME[@] asks an indexed array for any element, an associative one
# for its key @ (or * for [*]), and a scalar whether it is set -- tsh called
# A[@] set for any assoc with keys and a set scalar's unset (finding 424).
exec 2>&1
typeset -A A; A[a]=1; typeset -a a; a[1]=1; s1=foo; s2=
[ -v A[@] ] && echo A || echo "A unset"; [ -v a[@] ] && echo a || echo "a unset"
[ -v s1[@] ] && echo s1 || echo "s1 unset"; [ -v s2[@] ] && echo s2 || echo "s2 unset"
[ -v s3[@] ] && echo s3 || echo "s3 unset"
A[@]=x; [ -v A[@] ] && echo A2 || echo "A2 unset"; [ -v A[*] ] && echo A3 || echo "A3 unset"
[ -v a[*] ] && echo a2 || echo "a2 unset"; [ -v s1[*] ] && echo s12 || echo "s12 unset"
typeset -a e; [ -v e[@] ] && echo e || echo "e unset"
[[ -v s1[@] ]] && echo s13 || echo "s13 unset"
