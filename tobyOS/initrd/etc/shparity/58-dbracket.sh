# `[[ ... ]]`, the conditional command.
#
# /bin/tsh used to spawn `/bin/[[` and report 127 for every conditional
# expression in existence. It cannot be a builtin: <, >, &&, || and the
# parentheses mean something different inside it than they do outside, so
# the tokenizer would have turned `[[ a < b ]]` into a redirection first.
x=hello
empty=
[[ -n $x ]] ; echo A=$?
[[ -z $empty ]] ; echo B=$?
[[ $x == hello ]] ; echo C=$?
[[ $x == h* ]] ; echo D=$?
[[ $x == 'h*' ]] ; echo E=$?
[[ $x != world ]] ; echo F=$?
[[ a < b ]] ; echo G=$?
[[ b > a ]] ; echo H=$?
[[ 3 -lt 4 ]] ; echo I=$?
[[ -n $x && -n $x ]] ; echo J=$?
[[ -z $x || -n $x ]] ; echo K=$?
[[ ! -z $x ]] ; echo L=$?
[[ ( -n $x ) && ( 1 -eq 1 ) ]] ; echo M=$?
spaced='two words'
[[ -n $spaced ]] ; echo N=$?
[[ $spaced == 'two words' ]] ; echo O=$?
[[ -n x &&
   -n y ]] ; echo P=$?
if [[ $x == hello ]]; then echo Q=yes; fi
echo done
