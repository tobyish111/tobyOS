# `for w` WITH NO `in` IS `for w in "$@"` (batch 13): each parameter whole,
# nothing in it expanded again. tsh joined the parameters and PARSED the
# result -- a parameter holding $(...) RAN, one holding a quote was
# "unmatched double quote" -- and globbed each one. (logs/b13 q33 q35)
touch ax bx 'a*x'
set -- 'a*x' '$(touch made)' '"' "'" 'a b' ''
for w; do printf '[%s]' "$w"; done; echo
[ -e made ] && echo made || echo not-made
f() { for w; do printf '<%s>' "$w"; done; echo; }
f 'a*x' '*' "x y"
set --
for w; do echo never; done
echo end
