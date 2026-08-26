# POSIX 2.9.5 -- function definitions.
# A function gets its OWN positional parameters but shares everything else
# with the caller; $0 is unchanged; `return` sets the status; and a function
# may be redefined, recurse, and be called before the definition is reached
# textually as long as it is defined before it runs.
greet() { echo "1:hello $1"; }
greet world
greet "two words"
greet

argc() { echo "2:n=$# [$1][$2]"; }
argc
argc a
argc a b
argc "a b"

set -- outer1 outer2
inner() { echo "3:inside=[$1][$2] n=$#"; }
inner in1 in2
echo "4:after=[$1][$2] n=$#"

fwd() { later; }
later() { echo "5:defined-later"; }
fwd

rc() { return 4; }
rc; echo "6=$?"
rc0() { return 0; }
rc0; echo "7=$?"
implicit() { false; }
implicit; echo "8=$?"

fact() {
    if [ "$1" -le 1 ]; then echo 1; return; fi
    prev=$(fact $(( $1 - 1 )))
    echo $(( $1 * prev ))
}
echo "9=$(fact 5)"

shared=before
mutate() { shared=after; }
mutate
echo "10=$shared"

redef() { echo "11:first"; }
redef
redef() { echo "11:second"; }
redef

out() { echo "12:redirected"; }
out > fnout.txt
while read l; do echo "13:$l"; done < fnout.txt

nested_call() { echo "14:$(greet inner-sub)"; }
nested_call
echo end
