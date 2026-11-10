# cmdsubq.sh: a quoted `|` (and other operators) inside $( ) inside
# double quotes -- tsh said "bad command substitution" for
#   echo "x: $(printf 'a\nb\n' | tr "\n" "|")"
exec 2>&1
echo "q1: $(printf 'a\nb\n' | tr "\n" "|")"
echo "q2: $(echo "a|b")"
echo "q3: $(echo "a;b" ; echo "c&d")"
echo "q4: $(echo ")" )"
echo "q5: $(echo 'x|y' | tr "|" "-")"
v="$(echo "in" | tr "i" "o")"; echo "q6: $v"
echo "q7: $(case x in x) echo "c|x";; esac)"
echo "q8: $(case x in x) echo a | tr a b;; esac)"
echo q9: $(case x in x) echo a | tr a b;; esac)
u=; echo "q10: ${u:-"a|b"}"
echo "q11: `echo "a|b"`"
echo "q12: $(echo "$(echo "x|y")")"
echo "q13: a" | tr a b
echo "q14 $(echo a)" | tr a b
echo "$(echo "q15|")" | tr '|' '!'
echo "q16: ${u:-"$(echo "p|q")"}" | tr p P
echo "q17: $((1|2)) $(( (4|1) ))" | tr 5 F
f() { echo "q18: $(echo "f|g")"; }; f | tr f F
echo "q19: $(echo "a|b" | while read l; do echo "<$l>"; done)"
echo end
