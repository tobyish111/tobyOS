echo "$SHELLOPTS"
set -x; echo "$SHELLOPTS" >&2; set +x
set -o pipefail; echo "$SHELLOPTS"
SHELLOPTS=x; echo st=$?
unset BASHOPTS; echo st=$?
test -v SHELLOPTS && echo set
declare -p SHELLOPTS
echo "$-"
set -E; echo "$-"
shopt -s nullglob; case :$BASHOPTS: in *:nullglob:*) echo has-nullglob;; esac
