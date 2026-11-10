# Under set -k an assignment word anywhere on the line is the command's, and
# sees the assignments before it as a prefix word does: with $ECHO empty,
# `HOME=/a/b/c $ECHO a=$HOME` sets a to /a/b/c; any other word sees none of
# them (finding 388). tsh dropped them at the first word that was no
# assignment.
exec 2>&1
export HOME=/usr/chet; set -k
HOME=/a/b/c $ECHO a=$HOME c=9
echo $HOME $c $a
f() { echo "f $v $w"; }; v=1 f w=$v
echo b=1 x c=$b
set +k
