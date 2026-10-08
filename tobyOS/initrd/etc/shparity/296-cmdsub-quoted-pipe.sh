# A `$( )` INSIDE DOUBLE QUOTES HAS QUOTES OF ITS OWN, and a `|` in them
# is no pipe: `echo "x: $(echo "a|b")"` is one command. tsh's pipeline
# splitter ended the string at the inner `"`, cut the line at the `|` and
# said "bad command substitution" twice. The `)` of a case pattern inside
# a substitution does not end it either, quoted or not (logs/b16 cmdsubq).
exec 2>&1
echo "q1: $(printf 'a\nb\n' | tr "\n" "|")"
echo "q2: $(echo "a|b")"
echo "q5: $(echo 'x|y' | tr "|" "-")"
echo "q7: $(case x in x) echo "c|x";; esac)"
echo "q8: $(case x in x) echo a | tr a b;; esac)"
echo q9: $(case x in x) echo a | tr a b;; esac)
u=; echo "q10: ${u:-"a|b"}"
echo "q11: `echo "a|b"`"
echo "q12: $(echo "$(echo "x|y")")"
echo "q14 $(echo a)" | tr a b
echo "$(echo "q15|")" | tr '|' '!'
echo "q16: ${u:-"$(echo "p|q")"}" | tr p P
f() { echo "q18: $(echo "f|g")"; }; f | tr f F
echo "q19: $(echo "a|b" | while read l; do echo "<$l>"; done)"
echo end
