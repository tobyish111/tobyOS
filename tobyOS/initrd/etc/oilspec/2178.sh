case $SH in ash) return ;; esac  # yash and ash don't implement this

unprintable=$'\xff'
printf '%q\n' "$unprintable"

# bash and zsh agree
