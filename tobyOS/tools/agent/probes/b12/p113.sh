mkdir -p g54 && cd g54 || exit 1
touch foo=1 foo=2 a=x
echo foo=*
echo a=*
printf '%s\n' x=* foo=? 'foo=*'
v=foo=*; echo $v
x=foo=* ; echo "x=$x"
export y=foo=*; echo "y=$y"
f() { local z=foo=*; echo "z=$z"; }; f
declare d=foo=*; echo "d=$d"
w=foo=* env | grep '^w='
a=(foo=*); echo "a=${a[*]}"
for i in foo=*; do echo "for $i"; done
case foo=1 in foo=*) echo case;; esac
echo "q=foo=*" x=foo=?
cmd=export; $cmd e=foo=*; echo "e=$e"
cd .. && rm -rf g54
