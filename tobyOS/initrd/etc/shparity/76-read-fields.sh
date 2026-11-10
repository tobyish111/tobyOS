# POSIX read: exit status and field splitting at the edges. A partial line
# at EOF still assigns the variable but returns nonzero; extra fields
# collapse into the LAST variable with internal separators kept; leading and
# trailing IFS whitespace is stripped; backslash-newline joins unless -r.
printf 'abc' | { read x; echo "1=$?:$x"; }
printf 'a b c d\n' | { read x y; echo "2=[$x][$y]"; }
printf '  a  b  \n' | { read x y; echo "3=[$x][$y]"; }
printf 'a\\\nb\n' | { read x; echo "4=[$x]"; }
printf 'a\\tb\n' | { read -r x; echo "5=[$x]"; }
printf 'p:q:r\n' | { IFS=: read x y; echo "6=[$x][$y]"; }
printf 'hello there\n' | { read; echo "7=[$REPLY]"; }
printf ':a::b:\n' | { IFS=: read a b c d; echo "8=[$a][$b][$c][$d]"; }
printf '' | { read x; echo "9=$?:[$x]"; }
echo end
