# POSIX 2.9.4.1/2.9.4.2 -- ( ) runs in a SUBSHELL environment, { } runs in the
# CURRENT one. The difference is invisible until something assigns, cds, or
# exits, at which point it decides whether the change escapes.
x=outer
(x=sub); echo "1=$x"
{ x=grp; }; echo "2=$x"
x=outer

(unset x); echo "3=[$x]"
{ unset x; }; echo "4=[$x]"
x=outer

(exit 5); echo "5=$?"
{ true; }; echo "6=$?"

y=0
(y=1; echo "7-inside=$y")
echo "8-outside=$y"

{ y=2; echo "9-inside=$y"; }
echo "10-outside=$y"

> marker
(cd /etc; echo "11=${PWD##*/}")
echo "12=$( [ -f marker ] && echo cwd-unchanged )"

(echo a; echo b)
{ echo c; echo d; }

f() { echo "13-fn"; }
(f)
{ f; }

( (echo "14-nested") )
{ { echo "15-nested-grp"; }; }
echo "16=$( (echo sub-in-cmdsubst) )"
echo end
