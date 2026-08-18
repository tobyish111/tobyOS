# This is lame behavior: it does a conversion to 0 first for any string
shopt -u strict_arith || true
[[ a -eq a ]] && echo true
[[ a -eq b ]] && echo true
