x="FOO'BAR spam\"eggs"
eval "new=${x@Q}"
test "$x" = "$new" && echo OK
