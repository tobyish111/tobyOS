# no 64-bit integers
case $SH in mksh) exit ;; esac

echo -n 'before '; ulimit -f

# 1 << 63 overflows signed int

lim=$(( (1 << 62) + 1 ))
#echo lim=$lim

# bash detects that this is out of range
# so does osh-cpp, but not osh-cpython

ulimit -f $lim
echo -n 'after '; ulimit -f
