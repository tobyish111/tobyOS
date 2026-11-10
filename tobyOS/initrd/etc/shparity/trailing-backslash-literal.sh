# A backslash that ends a string the shell reads -- eval, a -c string, a
# backquoted command, a sourced file -- is a literal backslash; tsh said
# "trailing escape" (finding 438).
exec 2>&1
eval 'printf "%s\n" a\'
x=`printf '%s' \\`; echo "[$x]"
qpath='\/tmp\/foo\/bar'; printf "%s\n" ${qpath//"`printf '%s' \\`"/}
"$BASH" -c 'echo b\'
d=$(mktemp -d); printf 'echo c\\' > "$d/s.sh"; . "$d/s.sh"; rm -rf "$d"
