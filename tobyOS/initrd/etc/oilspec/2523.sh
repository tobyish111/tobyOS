var='$foo'
echo 1 "${var#$foo}"
echo 2 "${var#\$foo}"

var='}'
echo 10 "${var#}}"
echo 11 "${var#\}}"
echo 12 "${var#'}'}"
echo 13 "${var#"}"}"
