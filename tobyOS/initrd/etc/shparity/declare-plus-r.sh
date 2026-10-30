# declare +r of a readonly name is refused, "declare: x: readonly
# variable", status 1 -- through a reference, its target's -- and so is +n
# of a readonly reference with a target; one with no target takes +n and
# +r is a no-op there (finding 346). tsh took +r for a no-op, and +n off.
{
( readonly x=1; declare +r x; echo "rc=$?"; declare -p x; y=2; declare +r y; echo "rc=$?" )
( bar=one; declare -rn f=bar; declare +r f; echo "rc=$?"; declare -p f bar; declare +r -n f; echo "rc=$?"; declare +n f; echo "rc=$?"; declare -p f )
( b2=x; typeset -n f2=b2; readonly b2; typeset +r f2; echo "rc=$?"; typeset -p f2 b2 )
( fn() { typeset b4=four; typeset -n -r f4=b4; typeset +n f4; echo "rc=$?"; typeset -p f4; }; fn )
( g() { declare -r -n f5; declare -p f5; declare +n f5; declare -p f5; }; g )
} 2>&1
