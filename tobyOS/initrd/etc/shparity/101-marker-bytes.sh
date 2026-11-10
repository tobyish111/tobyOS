# The four byte values the word machinery uses as IN-BAND MARKERS, asked
# for as data.
#
#   0x01 SHELL_ARG_MARK      -- the "$@" word boundary
#   0x02 SHELL_NOSPLIT_MARK  -- "do not field-split from here"
#   0x03 SHELL_GLOB_ESC      -- "the next byte is ordinary, not a glob char"
#   0x04 SHELL_DATA_ESC      -- "the next byte is data, not one of the above"
#
# A script may legitimately ask for any of them, and $'\1' put SHELL_ARG_MARK
# straight into the word buffer, where the next scan read it as an argument
# boundary: `$'\001'` produced NOTHING. The escape hatch already existed
# (SHELL_DATA_ESC, and shell_append_data_n to apply it); the $'...' decoder
# in the word path just did not use it.
#
# Lengths and ordinary text, not the raw bytes, so the transcript stays
# comparable -- the point is that the byte survives as one character and
# does not perturb the words around it.

for e in '\1' '\2' '\3' '\4' '\5' '\377'; do
  s=$(printf '%b' "$e")
  :
done

a=$'\1'; echo "arg-mark len=${#a}"
b=$'\2'; echo "nosplit  len=${#b}"
c=$'\3'; echo "glob-esc len=${#c}"
d=$'\4'; echo "data-esc len=${#d}"
e=$'\377'; echo "high     len=${#e}"

# The marker byte must not split a word, and must not vanish from it.
w=$'x\1y'; echo "embedded len=${#w}"
x=$'\1\2\3\4'; echo "all-four len=${#x}"

# Around other words: the neighbours must be untouched.
set -- one $'\1' three
echo "argc=$# first=$1 third=$3"

# Concatenation with ordinary text on both sides.
p="A"$'\1'"B"; echo "concat len=${#p}"

# The same byte through printf %b, which is a different path to the value.
q=$(printf '%b' 'X\1Y'); echo "printf-b len=${#q}"

# And a quoted expansion of a variable holding one.
r="$a"; echo "requote len=${#r}"
echo end
