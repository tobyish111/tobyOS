# A local made with -I, or any new local under `shopt -s localvar_inherit`,
# starts as a copy of the variable it hides -- value, attributes but -n,
# elements -- and -I with nothing to copy is an ordinary local; -A over an
# inherited indexed array is refused under the function's name and then
# declare's (finding 385). tsh took -I and the option and started empty.
exec 2>&1
var=global; f() { local -I var; declare -p var; var=local; }; f; declare -p var
g() { local -I var; declare -p var; }; h() { local var=mid; g; }; h
unset var; g
x=5; k() { local -I -i x; x+=1; declare -p x; }; k; echo "[$x]"
shopt -s localvar_inherit
declare -A A=([k]=v); m() { local A; declare -p A; declare A=([Y]=Y); declare -p A; }; m
a=(X) s=X; n() { local -a a s; declare -p a s; local a+=(Y) s+=(Y); declare -p a s; }; n
declare -a w=(12); p() { declare -A w+=([0]=X); declare -p w; }; p
