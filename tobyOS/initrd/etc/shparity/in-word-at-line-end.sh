# The `in` of a header is the third word of `for NAME in`, `select NAME
# in` or `case WORD in`, and only that one. tsh took any `in` outside
# command position for it, so in a compound's body a line that ended in
# the word -- `echo in`, `grep in`, `cat f in` -- joined the next line with
# no `;` and the next command became its arguments (a loop whose
# condition was `echo in` then `false` never ended). And the NAME owes an
# `in` or a `do` that may stand on the next line: joining `for x` and `in
# a b` with a `;` ran the loop over "$@" (finding 626).
exec 2>&1
{ echo a; echo in
echo in2; }
f() {
  echo in
  echo next
}
f
if echo in
false; then echo yes; else echo no; fi
{ echo a | grep in
echo b; }
{ for x in a in
do echo "for $x"; done; }
for x
in c d; do echo "for $x"; done
case in in in) echo "case in";; esac
case b
in b) echo "case b";; esac
for in in e; do echo "for $in"; done
x=$(case a in
a) echo A;; esac); echo "$x"
