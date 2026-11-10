var=-f
[[ $var == -f ]] && echo true
[[ '-f' == $var ]] && echo true
