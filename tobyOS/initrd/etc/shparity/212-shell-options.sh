# set -x traces bare (`+ echo hi`), without the script-and-line prefix of
# a diagnostic; $SHELLOPTS and $BASHOPTS list the options that are on and
# are readonly. tsh had neither variable and put
# `x.sh: line 2: ` in front of every trace line.
exec 2>&1
set -x
echo hi
x=1
set +x
echo "$SHELLOPTS"
set -x; echo "$SHELLOPTS" >&2; set +x
set -o pipefail; echo "$SHELLOPTS"
SHELLOPTS=x; echo st=$?
unset BASHOPTS; echo st=$?
test -v SHELLOPTS && echo set
declare -p SHELLOPTS
shopt -s nullglob; case :$BASHOPTS: in *:nullglob:*) echo has-nullglob;; esac
