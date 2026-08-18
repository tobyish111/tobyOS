mkdir -p _tmp
PATH="_tmp:$PATH"
mkdir _tmp/cat

command -v _tmp/cat
echo status=$?
command -v cat
echo status=$?
