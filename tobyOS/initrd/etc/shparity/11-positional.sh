# Positional parameters: set --, shift, and the $* / $@ distinction. Passing
# "$@" through to a function is the single most common way a wrapper script
# preserves arguments containing spaces, so getting it wrong is load-bearing.
set -- alpha beta gamma
echo "count=$#"
echo "first=$1 third=$3"
echo "star=$*"
echo "at=$@"
shift
echo "after-shift=$1 count=$#"

set -- "one two" three
show() { echo "n=$# 1=[$1] 2=[$2]"; }
show "$@"
show $@

set -- a b c d
shift 2
echo "final=$# [$*]"
set --
echo "cleared=$#"
