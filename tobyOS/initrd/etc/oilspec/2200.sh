$SH -c '
export SHELLOPTS
set -x
echo 1
$SH -c "echo 2"
' 2>&1 | sed 's/.*sh /sh /g'
