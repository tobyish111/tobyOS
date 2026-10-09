# hist13.sh: the history entry a multi-line unit makes (finding 13) and
# HISTCONTROL / HISTIGNORE (finding 15), in a script with `set -o history`.
exec 2>&1
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
echo end
