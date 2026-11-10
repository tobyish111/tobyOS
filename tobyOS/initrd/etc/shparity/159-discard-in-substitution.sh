# A DISCARD inside $( ) -- failglob's miss here -- ends the substitution with
# status 1, as bash's subshell does. It used to throw away the rest of the
# CALLER's line: neither `a` nor `b` below was printed.
shopt -s failglob
x=$(echo /nonexist*.ZZ; echo in); echo "a [$x] $?"
echo "$(echo /nonexist*.ZZ; echo in2) tail"; echo "b $?"
y=$(echo ok; echo /nonexist*.ZZ); echo "c [$y] $?"
( echo /nonexist*.ZZ; echo in3 ); echo "d $?"
shopt -u failglob
echo done
