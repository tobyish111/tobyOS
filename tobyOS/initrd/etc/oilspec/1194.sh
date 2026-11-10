# Pipelines and AndOr are problematic

# THREE each
trap 'echo dbg $LINENO' DEBUG

false | false | false

false || false || false

! true

trap - DEBUG


# ONE EACH
trap 'echo err $LINENO' ERR

false | false | false

false || false || false

! true  # not run

echo ok
