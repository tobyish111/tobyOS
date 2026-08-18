# no 64-bit integers
case $SH in mksh) exit ;; esac

echo -n 'before '; ulimit -f

# 1 << 63 overflows signed int

# 512 is 1 << 9, so make it 62-9 = 53 bits

lim=$(( 1 << 53 ))
#echo $lim

# bash says this is out of range
ulimit -f $lim

echo -n 'after '; ulimit -f
