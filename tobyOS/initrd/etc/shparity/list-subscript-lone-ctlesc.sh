# A list's subscript that is a raw ^A alone, or two of them, is the byte
# itself to the arithmetic -- an error, the rest of the line dropped. A ^A
# beside other bytes still goes, as the escape of the byte after it. tsh
# took every ^A away and said "bad array subscript" (finding 613). The
# lines run as a script of their own: in an eval or a -c string the error
# goes past them to the top, 619's open question.
exec 2>&1
A=$(printf '\001')
S=/tmp/shp-613.$$
printf '%s\n' "a=( [$A]=x ); echo 'not reached 1'" 'echo "rc=$?"' \
    "b=( [$A$A]=x ); echo 'not reached 2'" 'echo "rc=$?"' \
    "c=( [1$A]=p [${A}2]=q ); declare -p c" > $S
"$BASH" $S 2>&1 | sed "s|$S|SCRIPT|"
rm -f $S
