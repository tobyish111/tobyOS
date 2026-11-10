# I guess dash and mksh treat unquoted [ as an invalid glob?
var='[foo]'
echo ${var#[}
echo ${var#"["}
echo "${var#[}"
echo "${var#"["}"
