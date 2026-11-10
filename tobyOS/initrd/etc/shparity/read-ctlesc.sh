# read's last variable is stripped as bash strips it: on bash's own copy of
# the line, where an escaped byte is CTLESC and the byte -- never the first
# byte, which may be that CTLESC (Oils 1137, "read bash bug": with IFS='x '
# `x\  \ ` gives b=$'\001'). The rest, as before.
r() { printf '%s\n' "$1" | { IFS=$2 read a b; printf '[%q] [%q]\n' "$a" "$b"; }; }
r 'x\  \ ' 'x '
r 'x\ ' 'x '
r 'x\  ' 'x '
r 'x \ ' 'x '
r 'xa\ ' 'x '
r 'x\ a\  ' 'x '
r 'x\ \ \ ' 'x '
r 'x\\ ' 'x '
r 'x\ y z\ ' 'x '
r 'xa b\ ' 'x '
r 'a \ ' ' '
r 'a \  \ ' ' '
r 'a b\ ' ' '
r 'a \x\ ' ' '
r 'a  \	\ ' $' \t'
r 'a,\ ,' ', '
r 'a,\ , ' ', '
r $'a x\001 ' 'x '
r $'a x\001' 'x '
r $'a x \001 ' 'x '
r $'x\177 ' 'x '
r 'x\  \ ' $'x \001'
printf '%s\n' 'x\  \ ' | { IFS='x ' read -r a b; printf '[%q] [%q]\n' "$a" "$b"; }
printf '%s\n' 'x\  \ ' | { IFS='x ' read a b c; printf '[%q] [%q] [%q]\n' "$a" "$b" "$c"; }
printf '%s\n' 'x\  \ ' | { IFS='x ' read a; printf '[%q]\n' "$a"; }
printf '%s\n' 'x\  \ ' | { IFS='x ' read -a arr; printf '[%q]' "${arr[@]}"; echo; }
