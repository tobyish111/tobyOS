f() { printf '<%s>' "$@"; echo; }
x='$HOME'; y='a b'; z='*'; w='$(echo hi)'; v='`echo bq`'; q="it's"; d='"dq"'; bs='a\b'
f "$x" "$y" "$z" "$w" "$v" "$q" "$d" "$bs" &
wait
printf '<%s>' "$x" "$y" "$z" "$w" "$v" "$q" "$d" "$bs" &
wait; echo
echo "$x" "$w" "$v" "$bs" &
wait
{ f "$w" "$q"; } &
wait
f "$x" "$w" | cat &
wait
( f "$v" "$bs" ) &
wait
f $y "$z" & wait
g() { local a="$1"; echo "[$a]"; }; g '$((1+2))' & wait
eval 'f "$w"' & wait
f 'semi;colon' 'amp&' 'pipe|x' 'lt<gt>' & wait
f '~' 'a=~' & wait
f "$(printf 'nl\nx')" & wait
