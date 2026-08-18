var='[a]foo[]'
echo ${var#[a]}
echo ${var#"[a]"}
echo "${var#[a]}"
echo "${var#"[a]"}"
