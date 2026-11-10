# bash 4 doesn't support negative indices or ranges

rm -f myhist
export HISTFILE=myhist

$SH --norc -i <<'EOF'

echo 42
echo 43
echo 44

history -a

history -d 1
echo status=$?

# Invalid integers
history -d -1
echo status=$?
history -d -2
echo status=$?
history -d 99
echo status=$?

case $SH in bash*) echo '^D' ;; esac

EOF


# bash-4.4 used to give more errors like OSH?  Weird
