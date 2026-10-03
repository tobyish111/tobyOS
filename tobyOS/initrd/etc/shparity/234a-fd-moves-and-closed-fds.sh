# N>&M- moves a descriptor; a write to a closed stdout is "write error:
# Bad file descriptor"; a read from a closed one is "read error: 0: ...";
# a program run with `<&-` gets fd 0 closed, not the shell's.
# (batch 12; probe p42 in C:\t\b12)
exec 2>&1
TMP=/tmp/p42.$$; mkdir -p $TMP
exec 5> "$TMP/f.txt"
echo hello5 >&5
exec 6>&5-
echo world5 >&5; echo "1 s=$?"
echo world6 >&6
exec 6>&-
cat "$TMP/f.txt"
exec 7> "$TMP/g.txt"
: 6>&7 7>&-
echo hello >&7; echo "2 s=$?"
: 6>&7-
echo world >&7; echo "3 s=$?"
exec 7>&-
cat "$TMP/g.txt"
mkdir -p $TMP/dir
mapfile x < $TMP/dir; echo "4 status=$? ${#x[@]}"
read y < $TMP/dir; echo "5 status=$?"
abc=def > /does/not/exist1; echo "6 s=$? abc=$abc"
unset abc
export abc=def > /does/not/exist2; echo "7 s=$? abc=$abc"
x2=1 2>/does/not/e3 true; echo "8 s=$? [$x2]"
cat <&8; echo "9 s=$?"
echo hi >&9-; echo "10 s=$?"
exec 3>$TMP/h.txt; echo a >&3; exec 4>&3-; echo b >&4; echo c >&3; echo "11 s=$?"; exec 4>&-; cat $TMP/h.txt
{ echo in-group; } 5>&1-; echo "12 s=$?"
exec 0<&-; read z; echo "13 s=$?"
rm -rf $TMP
