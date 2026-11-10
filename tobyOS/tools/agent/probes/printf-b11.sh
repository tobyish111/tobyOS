printf '[%5b][%-5b][%.2b]\n' 'a\tb' x 'abc'
printf '%d|%i|%u|%x|%o|%c|%s|\n' '' '' '' '' '' '' ''
printf '%d\n' ' '; echo st=$?
printf '%z'; echo st=$?
printf 'a%-zb'; echo st=$?
printf '%y'; echo st=$?
printf '%ld %hd %lld %jd %zd %td %Lf\n' 1 2 3 4 5 6 7.5
printf 'x\c y\n'; echo
printf '\e[0m\E' | od -An -c
printf '\x41\X42\x4a\xZ\n'
printf '%b\n' '\x41\X42\e' | od -An -c
printf '[%10.5(%Y-%m-%d)T]\n' 1558000000
printf '[%-12(%H)T]\n' 1558000000
printf -v 'a[1]' '%s' foo; declare -p a
declare -A m; printf -v 'm[k]' '%s' bar; declare -p m
printf -v 'bad name' x; echo st=$?
printf '%q\n' $'é'
printf 'é\U0001F600|%b\n' 'é'
