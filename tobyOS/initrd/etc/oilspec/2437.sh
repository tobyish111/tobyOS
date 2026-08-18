a=(1 2 3)
r='a'
echo ${!r@a}
r='a[0]'
echo ${!r@a}

declare -A d=([0]=foo [1]=bar)
r='d'
echo ${!r@a}
r='d[0]'
echo ${!r@a}
