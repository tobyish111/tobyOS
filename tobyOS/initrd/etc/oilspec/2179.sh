case $SH in ash) return ;; esac  # yash and ash don't implement this

unicode=$'\u03bc'
unicode=$'\xce\xbc'  # does the same thing

printf '%q\n' "$unicode"

# OSH issue: we have quotes.  Isn't that OK?
