# In POSIX mode an argument shaped like an assignment keeps its `~` after
# the `=` and the colons -- only an assignment expands them there: `echo
# foo=~:~` is foo=~:~ -- tsh expanded both (finding 499).
exec 2>&1
HOME=/home/xyz
echo foo=~:~
set -o posix
echo foo=~:~ x=~ "y"=~
foo=~:~; echo $foo
h=HOME=~
eval echo $h
set +o posix
echo foo=~:~
