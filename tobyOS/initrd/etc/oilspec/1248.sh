# mksh behaves differently in CI -- maybe when it's not connected to a
# terminal?
case $SH in mksh) echo mksh; exit ;; esac

$REPO_ROOT/spec/testdata/builtin-trap-int.sh



# Not sure why other shells differ here, but running the trap is consistent
# with interactive cases in test/bugs.sh
