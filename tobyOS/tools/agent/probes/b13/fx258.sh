d=/tmp/fxd.$$; rm -rf $d; mkdir $d; cd $d
# CHARACTERS, NOT BYTES, in the locale's encoding (batch 13). With no LANG
# the shells' musl locale is C.UTF-8: ${#v} and ${#a[i]} count characters,
# ${v:o:l} slices them. Under LC_ALL=C a byte is a character and one past
# 0x7f is unprintable, so @Q, declare -p and the set -x trace all quote it
# as $'\NNN'; in UTF-8 a printable character is itself. tsh counted bytes
# in ${#a[i]} and quoted only control bytes in @Q and the trace.
# (logs/b13 q1 q2)
s=$'héllo世'
a=("$s" $'é')
echo "len=${#s} elem=${#a[0]} ${#a[1]} cut=[${s:1:2}]"
printf '%s\n' "${s@Q}"
( set -x; : "$s" ) 2>&1
LC_ALL=C
echo "len=${#s} elem=${#a[0]} ${#a[1]}"
printf '%s\n' "${s@Q}" "${a[1]@Q}"
declare -p s
( set -x; : "$s" ) 2>&1
unset LC_ALL
echo end
