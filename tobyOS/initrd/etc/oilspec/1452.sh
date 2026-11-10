# Both are coerced to string!  It treats it more like an  UNQUOTED ${a[@]}.

a=('1 3' 5)
b=(1 2 3)
set -- 1 '3 5'
[[ "$@" = "${a[@]}" ]] && echo true
[[ "$@" = "${b[@]}" ]] || echo false
