# A line of an open $(( )) is arithmetic, where << is a shift and not a
# here-document -- `echo $((` then `1 << 8` then `))` hunted for a delimiter
# 8 to the end of the file (finding 441).
exec 2>&1
echo $((
  1 << 8
))
swap() {
        for arg in "$@"; do
                echo $((
                        ($arg & 65280) << 8 |
                        ($arg & 255) << 24
                ))
        done
}
swap 258 1
