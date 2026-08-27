# ${a[@]:off:len} slices the ELEMENTS; quoted it still yields one word each.
a=(a0 a1 a2 a3 a4)
echo "1=${a[@]:1}"
echo "2=${a[@]:1:2}"
echo "3=${a[*]:2:2}"
echo "4=[${a[@]:9:2}]"
echo "5=${a[@]:0:0}|"
for x in "${a[@]:1:2}"; do echo "w:[$x]"; done
set -- "${a[@]:3}"
echo "argc=$# 1=$1 2=$2"
echo end
