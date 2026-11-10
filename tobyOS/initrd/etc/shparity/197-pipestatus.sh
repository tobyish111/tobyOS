# PIPESTATUS: every pipeline, simple command, (( )), [[ ]] and subshell
# sets it; tsh had none.
exec 2>&1
true | false | true; echo "1 ${PIPESTATUS[@]}"
false; echo "2 ${PIPESTATUS[@]}"
(( 0 )); echo "3 ${PIPESTATUS[@]}"
[[ a == b ]]; echo "4 ${PIPESTATUS[@]}"
( exit 3 ); echo "5 ${PIPESTATUS[@]}"
{ false; } | true; echo "6 ${PIPESTATUS[@]}"
false | (exit 4) | true; echo "7 ${PIPESTATUS[*]} ${#PIPESTATUS[@]}"
