# POSIX 2.7.5/2.7.6 -- duplicating descriptors with <&n, and reusing high
# descriptor numbers. Splits three situations that the previous version of this
# case conflated: a high fd used on its own, a high fd used AFTER another was
# closed, and two high fds held open at the same time.
echo dup-target > src.txt

# A: a high descriptor on its own, nothing closed beforehand.
exec 7< src.txt
read a <&7
echo "A:[$a]"
exec 7<&-

# B: a different high descriptor, now that one has been closed.
exec 8< src.txt
read b <&8
echo "B:[$b]"
exec 8<&-

# C: two high descriptors held open simultaneously.
exec 4< src.txt
exec 5< src.txt
read c <&4
read d <&5
echo "C:[$c][$d]"
exec 4<&-
exec 5<&-

# D: reusing the SAME number after closing it.
exec 4< src.txt
read e <&4
exec 4<&-
exec 4< src.txt
read f <&4
echo "D:[$e][$f]"
exec 4<&-
echo end
