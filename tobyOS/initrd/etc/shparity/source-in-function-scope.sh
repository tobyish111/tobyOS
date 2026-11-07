# A file sourced from a function runs in the function's own scope -- `.`
# makes none of its own -- so its declare over the function's readonly local
# is refused, and its `declare v` without -g is the function's local. tsh
# took the sourced file for a scope of its own: the readonly local's list
# went into a new local of the same name, unrefused (finding 633).
exec 2>&1
echo 'declare x=(2) y=3; echo "the file went on"' > ./scope.src
f() { local -r x=(1); . ./scope.src; echo "f went on: $?"; declare -p x; }
f; echo "after f: $?"
declare -p y
echo 'declare v=inner; local w=lw' > ./scope2.src
g() { local v=outer; . ./scope2.src; echo "g sees $v $w"; }
g; echo "v after g: [${v-unset}] w: [${w-unset}]"
echo 'readonly r=1' > ./scope3.src
h() { local r=0; . ./scope3.src; r=2; echo "not reached"; }
h; echo "after h: $?"
