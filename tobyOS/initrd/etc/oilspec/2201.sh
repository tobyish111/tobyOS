# calling bash
$SH -c '
export SHELLOPTS
set -x
#echo SHELLOPTS=$SHELLOPTS
echo 1
bash -c "echo 2"
' 2>&1 | sed 's/.*sh /sh /g'
