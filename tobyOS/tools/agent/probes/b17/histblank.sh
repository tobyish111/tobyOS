# histblank.sh: how bash joins a multi-line unit's BLANK lines into its
# history entry (finding 13): after `do`, before `done`, two in a row, a
# line of blanks, in a group and before `then`. Each entry is printed
# between brackets so its spaces show.
exec 2>&1
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
