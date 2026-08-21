# A DATA BYTE THAT COLLIDES WITH AN IN-BAND MARKER.
# The word buffer reserves 0x01, 0x02 and 0x03; a byte that arrives from an
# expansion is data and has to survive capture, storage and re-expansion.
# (No `od` in the initrd, so the bytes are probed with the shell itself.)
has() {
  case $2 in
    *"$1"*) echo yes ;;
    *)      echo no ;;
  esac
}

one=$(printf '\001')
two=$(printf '\002')
three=$(printf '\003')

s=$(printf '.\001.')
echo len=${#s} $(has "$one" "$s")

s=$(printf '.\002.')
echo len=${#s} $(has "$two" "$s")

s=$(printf '.\003.')
echo len=${#s} $(has "$three" "$s")

# through a positional parameter and the $@ splitter
s=$(printf 'a\001b')
set -- "$s" second
echo count=$# len1=${#1} len2=${#2}
for a in "$@"; do echo item=${#a}; done

# unquoted, it still splits on IFS and not on the marker
set -- $s
echo split=$# len=${#1}

# a NUL is DROPPED, not kept and not terminating
s=$(printf '.\000.')
echo len=${#s}

s=$(printf '\001x')
echo len=${#s}
s=${s#?}
echo len=${#s} "$(has "$one" "$s")"
