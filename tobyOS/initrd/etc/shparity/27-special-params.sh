# POSIX 2.5.2 -- special parameters.
# $$ and $! are deliberately absent: they are not reproducible between two
# runs, and a corpus case that cannot be compared is worse than no case.
set -- alpha beta gamma
echo "count=$#"
echo "one=$1 two=$2 three=$3"
echo "missing=[$4]"
echo "star=$*"
echo "at=$@"
echo "star-q=[$*]"
echo "at-q=[$@]"
echo "zero-nonempty=$([ -n "$0" ] && echo yes)"

true;  echo "status-ok=$?"
false; echo "status-fail=$?"

set --
echo "empty-count=$#"
echo "empty-star=[$*]"
echo "empty-at=[$@]"

set -- "spaced arg" second
for a in "$@"; do echo "iter:[$a]"; done
for a in "$*"; do echo "star-iter:[$a]"; done
echo end
