# a4.sh: the findings audit, history -- 13 (a unit over several lines is
# joined by bash's cmdhist rules), 15 (HISTCONTROL, HISTIGNORE, history -p).
exec 2>&1
cd /tmp
cat > h13.sh <<'EOF'
set -o history
HISTFILE=/dev/null
for i in 1 2; do
  echo $i
done
echo a \
b
if true
then echo t
fi
x='q
r'
history | sed 's/^ *[0-9]*  //'
EOF
$SH h13.sh
cat > h15.sh <<'EOF'
set -o history
HISTFILE=/dev/null
HISTCONTROL=ignoredups:ignorespace
echo a
echo a
 echo sp
HISTIGNORE='ls*:echo ig*'
ls >/dev/null
echo ig1
echo b
history | sed 's/^ *[0-9]*  //'
EOF
$SH h15.sh
cat > h15p.sh <<'EOF'
set -o history
HISTFILE=/dev/null
echo one two
history -p '!!' '!e' 'x!$y' plain
echo "rc=$?"
EOF
$SH h15p.sh
rm -f h13.sh h15.sh h15p.sh
echo end
