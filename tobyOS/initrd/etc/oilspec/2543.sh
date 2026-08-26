case $SH in dash) exit ;; esac

shopt -s compat_array  # to refer to array as scalar

empty=()
a1=('')
a2=('' x)
a3=(3 4)
echo empty=${empty[@]-minus}
echo a1=${a1[@]-minus}
echo a1[0]=${a1[0]-minus}
echo a2=${a2[@]-minus}
echo a3=${a3[@]-minus}
echo ---

echo empty=${empty[@]+plus}
echo a1=${a1[@]+plus}
echo a1[0]=${a1[0]+plus}
echo a2=${a2[@]+plus}
echo a3=${a3[@]+plus}
echo ---

echo empty=${empty+plus}
echo a1=${a1+plus}
echo a2=${a2+plus}
echo a3=${a3+plus}
echo ---

# Test quoted arrays too
argv.py "${empty[@]-minus}"
argv.py "${empty[@]+plus}"
argv.py "${a1[@]-minus}"
argv.py "${a1[@]+plus}"
argv.py "${a1[0]-minus}"
argv.py "${a1[0]+plus}"
argv.py "${a2[@]-minus}"
argv.py "${a2[@]+plus}"
argv.py "${a3[@]-minus}"
argv.py "${a3[@]+plus}"
