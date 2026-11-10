# A BACKGROUNDED COMMAND IS A SUBSHELL, INCLUDING ITS EXPANSIONS.
#
# `${bar=2}` assigns, and the assignment belongs to the child. tsh
# expanded the whole line in this process and only forked afterwards, by
# which time bar was set here too. The `&` has to be found in the SOURCE.
echo ${foo=1}
echo A=$foo
echo ${bar=2} &
wait
echo "B=[$bar]"
v=outer
{ v=inner; echo C=$v; } &
wait
echo "D=$v"
# `echo E & echo F` is deliberately NOT here: which of the two
# reaches stdout first is a race between the child and this
# process, and bash only wins it by being quick.
sleep 0 & echo G=$?
wait
echo done
