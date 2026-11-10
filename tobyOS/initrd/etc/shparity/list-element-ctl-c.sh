# A ^C in a list's element is data -- $'a\003b', or one written raw --
# and stays there. tsh took it for its own glob escape and dropped it
# (finding 566).
exec 2>&1
a=( $'a\003b' "x"$'\003'"y" ); declare -p a
printf '%s' "${a[0]}" | od -An -tx1
eval "b=( p$(printf '\003')q )"; declare -p b
