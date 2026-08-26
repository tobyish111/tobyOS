# bash makes these 3 read-only
{
  UID=xx $SH -c 'echo uid=$UID'

  EUID=xx $SH -c 'echo euid=$EUID'

  PPID=xx $SH -c 'echo ppid=$PPID'

} > out.txt

# bash shows that vars are readonly
# zsh shows other errors
# cat out.txt
#echo

grep '=xx' out.txt
echo status=$?
