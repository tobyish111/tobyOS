# declare -n refuses an element name and a name that is an array -- "NAME:
# reference variable cannot be an array", status 1, nothing changed -- after
# the value is checked as a name; a written (...) value makes the array; and
# a reference holds a name, so -i -l -u go (finding 326).
{
( declare -n 'x[3]=x'; echo "rc=$?"; x=(the browns); declare -n x=y; echo "rc=$? $x"; declare -p x )
( declare -A A=([k]=v); declare -n A=y; echo "rc=$?"; declare -p A )
( declare -a e; declare -n e=y; echo "rc=$?"; declare -p e )
( declare -a b=(zero); declare -n b='(one two)'; echo "rc=$?"; declare -p b )
( declare -n array=(one two three); echo "rc=$?"; declare -p array )
( y=42; declare -i x=1; declare -n x=y; echo "$x"; declare -p x; declare +n x; echo "$x"; declare -p x )
( Y=Q; declare -l a=Abc; declare -n a=Y; declare -p a; declare -u b=x; declare -n b=Y; declare -p b )
} 2>&1
