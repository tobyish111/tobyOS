# This test depends on the exact version
# bind -l | sort > _tmp/this-shell-bind-l.txt
# comm -23 $REPO_ROOT/spec/testdata/bind/bind_l_function_list.txt _tmp/this-shell-bind-l.txt

# More relaxed test
bind -l | grep accept-line
