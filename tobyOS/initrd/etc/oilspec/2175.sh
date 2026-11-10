case $SH in ash) return ;; esac  # yash and ash don't implement this

newline=$'one\ntwo'
printf '%q\n' "$newline"

quoted="$(printf '%q\n' "$newline")"
restored=$(eval "echo $quoted")
test "$newline" = "$restored" && echo roundtrip-ok
