# printf as bash's: %b takes a width and a precision; an empty numeric
# operand is 0; a missing or unknown conversion stops with status 1; \x,
# \e and \u/\U work in the format; %(fmt)T takes a precision; %c of '' is a
# NUL; `-v a[1]` assigns an element. tsh did none of these.
exec 2>&1
printf '[%5b][%-5b][%.2b]\n' 'a\tb' x 'abc'
printf '%d|%i|%u|%x|%o|%s|\n' '' '' '' '' '' ''
printf '%d\n' ' '; echo st=$?
printf '%z'; echo st=$?
printf 'a%-zb'; echo st=$?
printf '%y'; echo st=$?
printf 'p%sq%' x 2>/dev/null; echo " st=$?"
printf '%ld %hd %lld %jd %zd %td %Lf\n' 1 2 3 4 5 6 7.5
printf 'x\c y\n'; echo
printf '\e[0m\E' | od -An -c
printf '\x41\X42\x4a\xZ\n' 2>/dev/null
printf '%b\n' '\x41\X42\e' | od -An -c
printf '\u2620|\U0000065f|\u41|%b\n' '\u03bc'
printf '[%10.5(%Y-%m-%d)T]\n' 1558000000
printf '[%-12(%H)T]\n' 1558000000
printf -v 'a[1]' '%s' foo; declare -p a
declare -A m; printf -v 'm[k]' '%s' bar; declare -p m
printf -v 'bad name' x; echo st=$?
printf '%b %s\n' 'a\cb' x y; echo st=$?
