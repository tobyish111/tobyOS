case $SH in ash) return ;; esac

# Hm bash/mksh/zsh understand these.  They are doing decoding and error
# recovery!  inspecting the bash source seems to confirm this.
unicode=$'\xce'
printf '%q\n' "$unicode"

unicode=$'\xce\xce\xbc'
printf '%q\n' "$unicode"

unicode=$'\xce\xbc\xce'
printf '%q\n' "$unicode"

case $SH in mksh) return ;; esac  # it prints unprintable chars here!

unicode=$'\xcea'
printf '%q\n' "$unicode"
unicode=$'a\xce'
printf '%q\n' "$unicode"
