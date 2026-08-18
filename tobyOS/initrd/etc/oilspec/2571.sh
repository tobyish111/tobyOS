check_eq() {
  [ "$1" = "$2" ] || { echo "$1 vs $2"; }
}
check_expand() {
  val=$(eval "echo \"$1\"")
  [ "$val" = "$2" ] || { echo "$1 -> expected $2, got $val"; }
}
check_err() {
  e="$1"
  msg=$(eval "$e" 2>&1) && echo "bad success: $e"
  if test -n "$2"; then 
    if [[ "$msg" != $2 ]]; then
      echo "Expected error: $e"
      echo "Got error     : $msg"
    fi
  fi
}
# Nearly everything in manual section 3.5.3 "Shell Parameter Expansion"
# is allowed after a !-indirection.
#
# Not allowed: any further prefix syntax.
x=xx; xx=aaabcc
xd=x
check_err '${!!xd}'
check_err '${!!x*}'
a=(asdf x)
check_err '${!!a[*]}'
check_err '${!#x}'
check_err '${!#a[@]}'
# And an array reference binds tighter in the syntax, so goes first;
# there's no way to spell "indirection, then array reference".
check_expand '${!a[1]}' xx
b=(aoeu a)
check_expand '${!b[1]}' asdf  # i.e. like !(b[1]), not (!b)[1]
#
# Allowed: apparently everything else.
y=yy; yy=
check_expand '${!y:-foo}' foo
check_expand '${!x:-foo}' aaabcc

check_expand '${!x:?oops}' aaabcc

check_expand '${!y:+foo}' ''
check_expand '${!x:+foo}' foo

check_expand '${!x:2}' abcc
check_expand '${!x:2:2}' ab

check_expand '${!x#*a}' aabcc
check_expand '${!x%%c*}' aaab
check_expand '${!x/a*b/d}' dcc

# ^ operator not fully implemented in OSH
#check_expand '${!x^a}' Aaabcc

p=pp; pp='\$ '
check_expand '${!p@P}' '$ '
echo ok
