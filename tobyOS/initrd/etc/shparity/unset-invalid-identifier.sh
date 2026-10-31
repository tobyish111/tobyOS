# Under -v, an unset word that is neither a name nor NAME[...] -- the
# halves `a[x` and `y]` of a split key -- is "not a valid identifier",
# status 1; without -v only a function of that name goes, silently --
# tsh said nothing either way (finding 486).
exec 2>&1
declare -A a
k='$(echo foo)'
a[$k]=1
unset -v a[$k]; echo "rc=$?"
unset -v a["$k"]
declare -p a
k2='x y'
a[$k2]=2
unset -v a[$k2]; echo "rc=$?"
declare -p a
f-g() { echo in-fg; }
unset f-g; echo "rc=$?"; type -t f-g; echo "t=$?"
h-i() { :; }
unset -v h-i; echo "rc=$?"; type -t h-i
unset 'x y' 'p[1' ''; echo "rc=$?"
unset nosuch[1] nosuch; echo "rc=$?"
