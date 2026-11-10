# POSIX 2.7 -- descriptor redirection, and the ordering rule that catches
# everyone: redirections are applied left to right, so `>f 2>&1` sends both
# streams to the file, while `2>&1 >f` sends stderr to the ORIGINAL stdout.
# Only stdout is compared by the gate, so each case routes what it wants
# measured onto fd 1.
echo to-stdout
echo to-stderr 1>&2

echo both > o1.txt 2>&1
while read l; do echo "a:$l"; done < o1.txt

{ echo out; echo err 1>&2; } > o2.txt 2>&1
while read l; do echo "b:$l"; done < o2.txt

{ echo out2; echo err2 1>&2; } 2>&1 > o3.txt
while read l; do echo "c:$l"; done < o3.txt

exec 3> o4.txt
echo via-fd3 >&3
exec 3>&-
while read l; do echo "d:$l"; done < o4.txt

echo dup-target > o5.txt
exec 4< o5.txt
while read l <&4; do echo "e:$l"; done
exec 4<&-
echo end
