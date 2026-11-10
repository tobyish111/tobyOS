# Under a null IFS, outside double quotes, an array's pattern removal is
# one word: its elements joined by a space, each space inside an element
# led by a ^A (bash's own quoting byte); only ${a[*]#p} in an assignment
# runs them together, and with IFS set [@]'s joins with IFS's first
# character. tsh made a field of each element (finding 557).
exec 2>&1
a=(a 'b c' d); declare -A m=([k]='p q')
IFS=
printf '<%s>' ${a[*]#x} ${a[@]%x} ${a[*]##}; echo
printf '%s' ${a[*]#x} | od -An -c
printf '%s' ${m[*]#x} | od -An -c
y=${a[*]#x}; echo "[$y]"
y=${a[@]#x}; printf '%s' "$y" | od -An -c
printf '<%s>' "${a[@]#x}" ${a[*]#} ${a[*]%}; echo
IFS=-
y=${a[@]#x}; echo "[$y]"
printf '<%s>' ${a[@]#x}; echo
