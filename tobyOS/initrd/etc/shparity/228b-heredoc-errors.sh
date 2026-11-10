# An expansion error in a here-document fails the command (status 1) and
# nothing else; LINENO counts the body's lines.
# (batch 12; probe p20 in C:\t\b12)
exec 2>&1
a=(1)
( set -u; cat <<E
$undefined_v
E
echo "1 s=$?" )
echo "1 sub=$?"
( set -e; cat <<E
${a[]}
E
echo "2 not reached" )
echo "2 sub=$?"
( cat <<E >/tmp/p20.out
${a[]}
E
echo "3 s=$? $(ls /tmp/p20.out 2>/dev/null)" )
( cat >/tmp/p20.b <<E 2>/tmp/p20.err
${a[]}
E
echo "4 s=$? [$(cat /tmp/p20.err)]" )
( cat <<E1 <<E2
one
E1
${a[]}
E2
echo "5 s=$?" )
( if cat <<E
${a[]}
E
then echo "6 then"; else echo "6 else $?"; fi )
( while cat <<E
${u:?gone}
E
do echo "7 body"; break; done; echo "7 s=$?" )
rm -f /tmp/p20.out /tmp/p20.b /tmp/p20.err
