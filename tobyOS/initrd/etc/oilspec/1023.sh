# TODO: in YSH, this should be deprecated
case $SH in dash|ash) exit ;; esac

show_bytes() {
  od -A n -t x1
}
twomu=$'\u03bc\u03bc'
printf '[%s]\n' "$twomu"

# Hm this cuts off a UTF-8 character?
printf '%c' "$twomu" | show_bytes
