case $SH in dash|mksh|ash) exit ;; esac

x='a b'

builtin declare c=$x
echo $c

\builtin declare d=$x
echo $d

'builtin' declare e=$x
echo $e

b=builtin
$b declare f=$x
echo $f

b=b
${b}uiltin declare g=$x
echo $g
