# bash does print syntax errors but somehow it exits 0

$SH -c 'echo `echo "`'
echo status=$?
$SH -c 'echo `echo \\\\"`'
echo status=$?
