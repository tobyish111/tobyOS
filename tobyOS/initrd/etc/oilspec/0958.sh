case $SH in dash|ash) exit ;; esac

builtin typeset s=typeset
echo s=$s

builtin export s=export
echo s=$s

builtin readonly s=readonly
echo s=$s

echo --

builtin builtin typeset s2=typeset
echo s2=$s2

builtin builtin export s2=export
echo s2=$s2

builtin builtin readonly s2=readonly
echo s2=$s2
