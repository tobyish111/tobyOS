# The word of ${v=W}, a pattern and a replacement are one string each:
# under a null IFS their $* runs together and their $@ joins with a space.
# tsh ran $@ together in ${v=W}, and left its field marks in a
# replacement (finding 562).
exec 2>&1
set -- p 'q r'; v=v
IFS=
unset z; y=${z=$@}; echo "[$y][$z]"
unset z; y=${z=$*}; echo "[$y][$z]"
y=${v/v/$@}; echo "[$y]"
y=${v/v/$*}; echo "[$y]"
printf '<%s>' ${v/v/$@}; echo
v=abcd; set -- b c
printf '<%s>' ${v#a$*} ${v/$*/X}; echo
