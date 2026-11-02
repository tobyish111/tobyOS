# A scalar a function made local, declared local again as an array -- or
# given an element by local or declare -- is no longer its value:
# `local x=s; local -a x` is x=(), and `local -a x[1]=one` x=([1]="one").
# tsh made the scalar element 0 (finding 576).
exec 2>&1
f() { local x=s; local -a x; declare -p x; }; f
f() { local x=s; local x[1]=one; declare -p x; }; f
f() { local x=s; declare -a x[1]=one; declare -p x; }; f
f() { local x; local -a x[1]=one; declare -p x; }; f
f() { local -a x=(p q); local -a x[1]=one; declare -p x; }; f
x=s; declare -a x[1]=one; declare -p x
