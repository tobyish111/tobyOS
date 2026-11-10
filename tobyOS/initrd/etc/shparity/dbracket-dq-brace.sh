# Inside [[ ]] a ${ }, $( ) or backquote within double quotes has quotes
# of its own: the word reader ended the quoted span at the inner `"`
# ("conditional binary operator expected", "unterminated ${...}"; finding
# 303).
x=x
[[ "${u:-"a b"}" == "a b" ]] && echo default
[[ "${x+"a"}" == a ]] && echo alternate
[[ -z "${x+"$@"}" ]] && echo empty-at
[[ "$(echo "a b")" == "a b" ]] && echo comsub
[[ "`echo "q"`" == q ]] && echo backquote
[[ abc == "${u:-"a"}"* ]] && echo pattern
[[ abc =~ "${u:-"b"}" ]] && echo regex
[[ "${u:-'q'}" == "'q'" ]] && echo single
echo done
