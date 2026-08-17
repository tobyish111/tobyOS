# POSIX 2.9.3 -- AND-OR lists. && and || have EQUAL precedence and associate
# left to right, which is not what most people assume and is exactly where a
# hand-rolled evaluator invents precedence that is not there.
true  && echo "1-and-ok"
false || echo "2-or-ok"
true  || echo "3-should-not-print"
false && echo "4-should-not-print"

true  && echo "5a" && echo "5b"
false && echo "6a" && echo "6b"
false || echo "7a" || echo "7b"
true  || echo "8a" || echo "8b"

# Left-to-right: `false && x || y` runs y, because (false && x) is false.
false && echo "9-no" || echo "9-yes"
true  && echo "10-yes" || echo "10-no"
true  || echo "11-no" && echo "11-yes"
false || echo "12-yes" && echo "12-also"

echo a; echo b
echo c; echo d;

if true && true;  then echo "13-both"; fi
if true && false; then echo "14-no"; else echo "14-else"; fi
if false || true; then echo "15-or"; fi
if ! false && true; then echo "16-neg"; fi

x=0
true && x=1
echo "17=$x"
false || x=2
echo "18=$x"
echo end
