# POSIX 2.6.2 -- parameter expansion defaults, BOTH forms.
# With a colon the test is "unset or null"; without one it is "unset" only.
# Scripts lean on that difference to distinguish an empty setting from a
# missing one, so a shell that treats the two forms alike is subtly wrong.
unset u
e=""
s=set

echo "1=[${u-default}]"
echo "2=[${e-default}]"
echo "3=[${s-default}]"
echo "4=[${u:-default}]"
echo "5=[${e:-default}]"
echo "6=[${s:-default}]"

echo "7=[${u+alt}]"
echo "8=[${e+alt}]"
echo "9=[${s+alt}]"
echo "10=[${u:+alt}]"
echo "11=[${e:+alt}]"
echo "12=[${s:+alt}]"

unset a1
echo "13=[${a1=assigned}]"
echo "14=[${a1}]"
a2=""
echo "15=[${a2=notused}]"
echo "16=[${a2:=nowused}]"
echo "17=[${a2}]"

echo "18=[${u-$s}]"
echo "19=[${u:-nested-$s}]"
echo end
