# A here-document inside a $( ) that runs over lines: its body is text,
# quotes, `#` lines and indentation included (batch 37:
# logs/b37/hdcomsub.sh). Read as shell, an apostrophe in it opened a string
# the reader hunted for to the end of the script.
c() { while IFS= read -r l; do printf '%s\n' "$l"; done; }
x=$(c <<EOF
it's here
EOF
)
echo "[$x]"
x=$(c <<'EOF'
it's $HOME
EOF
)
echo "[$x]"
echo "$(c <<EOF
don't
EOF
)"
x=$(c <<EOF
   indented line
# not a comment
  a "quote
EOF
)
echo "[$x]"
x=$(c <<-EOF
	tab'd
	EOF
)
echo "[$x]"
f() {
  local x
  x=$(c <<EOF
it's in f
  kept "spaces"
# kept hash
EOF
)
  echo "[$x]"
}
f
if true; then
  y=$(c <<\EOF
won't expand $HOME
EOF
)
  echo "[$y]"
fi
if false; then :
elif true; then
  y=$(c <<EOF
elif's
EOF
)
  echo "[$y]"
fi
i=0
while [ $i -lt 1 ]; do
  y=$(c <<EOF
loop's body
EOF
)
  echo "[$y]"; i=$((i+1))
done
for w in a; do
  y=$(c <<EOF
for's $w
EOF
)
  echo "[$y]"
done
case x in
x) y=$(c <<EOF
case's
EOF
)
   echo "[$y]";;
esac
x=$(c <<A; c <<B
a's
A
b's
B
)
echo "[$x]"
x=$(c <<EOF
it's
EOF
echo after-in
)
echo "[$x]"
echo $(echo $((1<<3)) $(( 2 << 1 )))
x=$(echo $((4<<1))
); echo "[$x]"
read -r l < <( c <<EOF
in a procsub
EOF
); echo "[$l]"
while IFS= read -r l; do echo "ps: $l"; done < <(c <<'EOF'
it's quoted
EOF
)
e=$(c <<EOF
contents
EOF)
echo "[$e]"
foo=$(c <<EOF
hi
EOF )
echo "[$foo]"
echo end
