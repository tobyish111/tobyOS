case $SH in ash) return ;; esac  # yash and ash don't implement %q

quotes=\'\"
printf '%q\n' "$quotes"

quoted="$(printf '%q\n' "$quotes")"
restored=$(eval "echo $quoted")
test "$quotes" = "$restored" && echo roundtrip-ok
