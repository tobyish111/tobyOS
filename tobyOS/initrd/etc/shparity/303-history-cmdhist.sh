# A UNIT OVER SEVERAL LINES IS ONE HISTORY ENTRY, joined as bash joins it
# (cmdhist), and HISTCONTROL and HISTIGNORE decide what is kept
# (findings 13 15; logs/b17 hist13 histblank).
exec 2>&1
# the shell under test: the gate sets no SH (probe3 does); $BASH is its own path
SH=${SH:-$BASH}
cd /tmp
f=/tmp/hist13.$$.sh
run() { $SH "$f" 2>&1 | sed 's/^ *[0-9]*  //'; }
cat > "$f" <<'EOF'
set -o history
HISTFILE=/dev/null
f()
{
  echo in-f
}
case x in
  x) echo cx ;;
  y) echo cy
     ;;
esac
while false
do

  echo never

done
for i in 1; do   # trailing comment
  echo $i
  # a comment line
  echo after
done
{ echo g1
  echo g2; }
echo p1 |
  cat
true &&
  echo andand
a=(one
   two)
s="multi
line"
x=$(echo c1
echo c2)
( echo sub
)
if true; then echo t; else
  echo e; fi
history
EOF
run
cat > "$f" <<'EOF'
set -o history
HISTFILE=/dev/null
HISTCONTROL=erasedups
echo e1
echo e2
echo e1
HISTCONTROL=ignoreboth
	echo tab-led
 echo space-led
echo dup
echo dup
HISTCONTROL=
HISTIGNORE='&:echo [ab]*:echo x\:y'
echo z
echo z
echo apple
echo cherry
echo x:y
HISTIGNORE='*'
echo hidden
HISTIGNORE=
history
EOF
run
rm -f "$f"
f=/tmp/histblank.$$.sh
run() { printf 'set -o history
HISTFILE=/dev/null
%s
history | sed "s/^ *[0-9]*  //; s/.*/[&]/"
' "$1" > "$f"; $SH "$f" 2>&1 | grep -v -e '^\[HISTFILE' -e '^\[history'; }
run 'while false
do

echo n
done'
run 'while false
do
echo n

done'
run 'while false
do


echo n
done'
run 'while false
do

echo n
done'
run '{
echo a

echo b
}'
run 'if true

then echo t
fi'
run 'for i in 1

do echo $i; done'
run 'echo a &&

echo b'
rm -f "$f"
echo end
