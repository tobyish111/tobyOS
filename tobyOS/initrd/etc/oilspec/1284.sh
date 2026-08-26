mkdir -p _tmp
PATH="_tmp:$PATH"
mkdir _tmp/cat

type -P _tmp/cat
echo status=$?
type -P cat
echo status=$?
