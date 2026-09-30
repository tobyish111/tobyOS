f=/tmp/fd12.$$
exec 12>"$f"; echo twelve >&12; exec 12>&-; cat "$f"
{ echo inner >&15; } 15>"$f"; cat "$f"
exec {fd}>"$f"; echo "fd>=10: $((fd>=10))"; echo via-var >&$fd; exec {fd}>&-; cat "$f"
exec 3>"$f"; exec 20>&3; echo dup20 >&20; exec 20>&- 3>&-; cat "$f"
cat <<< here 30<&0
echo x 2>&1 99>/dev/null
ls /proc/self/fd >/dev/null; echo done
f=/tmp/fdv.$$; g=/tmp/fdw.$$
exec {a}>"$f" {b}>"$g"; echo "a=$a b=$b"; echo A >&$a; echo B >&$b; exec {a}>&- {b}>&-; cat "$f" "$g"
exec {r}<"$f"; read -r line <&$r; echo "read [$line]"; exec {r}<&-
exec {rw}<>"$f"; echo "rw=$rw"; exec {rw}>&-
exec {d}>&1; echo "to dup" >&$d; exec {d}>&-
echo keep {k}>"$g"; echo "k=$k"; echo more >&$k; exec {k}>&-; cat "$g"
exec 11>"$f"; exec {n}>"$g"; echo "n=$n"; exec 11>&- {n}>&-
exec 25>"$f"; echo twentyfive >&25; exec 25>&-; cat "$f"
exec 40>"$f"; sh -c 'echo child >&40'; exec 40>&-; cat "$f"
( exec 50>"$f"; echo sub >&50 ); cat "$f"
rm -f "$f" "$g"; echo done
