case $SH in ash) return ;; esac  # yash and ash don't implement this

# bash does a weird thing and uses \

spaces='one two'
printf '%q\n' "$spaces"
