# The shell's 0, 1 and 2 are its process's own descriptors 0, 1 and 2:
# `exec >f` puts f at 1 -- the file's own descriptor goes -- and a group's
# or a function's redirection of 0-2 puts its file there while what it
# replaced waits at a number from 10 up until the redirection ends; a
# group's `>&-` closes 1. A command's own redirection is never the
# shell's. tsh's 0-2 stayed its own and the file sat at a number of its own:
# `exec >f; ls /proc/$$/fd` listed 0 1 2 255 3 (finding 618).
exec 2>&1
ls /proc/$$/fd > l0; tr '\n' ' ' < l0; echo
exec 3>&1
exec > o1; ls /proc/$$/fd > l1; exec >&3; tr '\n' ' ' < l1; echo
exec 2> o2; ls /proc/$$/fd > l2; exec 2>&3; tr '\n' ' ' < l2; echo
exec 0< /dev/null; ls /proc/$$/fd > l3; tr '\n' ' ' < l3; echo
{ ls /proc/$$/fd > l4; } >&-; tr '\n' ' ' < l4; echo
{ ls /proc/$$/fd > l5; } > o5; tr '\n' ' ' < l5; echo
f() { ls /proc/$$/fd > l6; }; f 2> /dev/null; tr '\n' ' ' < l6; echo
{ ls -l /proc/$$/fd/1 | sed 's/.*-> //' > l7; } > o7; sed 's|.*/||' l7
exec 3>&-
ls /proc/$$/fd > l8; tr '\n' ' ' < l8; echo
echo "still writing"
