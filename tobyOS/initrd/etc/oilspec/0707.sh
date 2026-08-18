left=$TMP/left
right=$TMP/right
touch $left $right

ln -f $TMP/left $TMP/hardlink

test $left -ef $left && echo same
test $left -ef $TMP/hardlink && echo same
test $left -ef $right || echo different

test $TMP/__nonexistent -ef $right || echo different
