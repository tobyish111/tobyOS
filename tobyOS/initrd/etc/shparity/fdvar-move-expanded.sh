# A `-` written after an expansion in <&WORD and >&WORD is the move
# operator: `{stdin}<&$fd0-` moves $fd0 to a new descriptor and closes it,
# where a value that ends in a dash is still a file name (finding 454).
exec 2>&1
swizzle() {
exec {fd0}<&0
exec {fd1}>&1
exec {stdin}<&$fd0-
exec {stdout}>&$fd1-
}
swizzle
echo $stdin $stdout
read line <&$stdin <<EOF
a
EOF
echo $line
echo $line >&$stdout
d=$(mktemp -d); cd "$d" || exit; z=7-; echo hi >&$z; ls; cd /; rm -rf "$d"
