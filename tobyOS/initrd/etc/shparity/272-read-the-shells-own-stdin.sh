# `read` and `mapfile` with nothing redirected read the SHELL'S OWN STDIN.
# /bin/tsh read fd 1 there -- a terminal answers on both, so every
# interactive use worked, but `echo ab | tsh -c 'read x'` and
# `tsh -c 'read x' < file` read nothing at all (logs/b13 q56 q57).
exec 2>&1
cd /tmp
echo ab | "$BASH" -c 'read x; echo "rc=$? [$x]"'
printf 'a\r\n' | "$BASH" -c 'read x; echo "len=${#x}"'
printf 'l1\nl2\nl3\n' | "$BASH" -c 'mapfile -t a; echo "${#a[@]} ${a[2]}"'
printf 'one two\n' > in272.txt
"$BASH" -c 'read x y; echo "[$x][$y]"' < in272.txt
"$BASH" -c 'readarray b; printf "%s" "${b[@]}"' < in272.txt
printf 'p\nq\n' | "$BASH" -c 'while read -r l; do echo "got $l"; done'
printf 'read v\nhello\necho "v=$v"\n' | "$BASH"
rm -f in272.txt
