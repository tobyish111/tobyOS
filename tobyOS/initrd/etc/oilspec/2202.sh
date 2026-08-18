#echo outside=$SHELLOPTS

# sed pattern to normalize spaces
normalize='s/[ \t]\+/ /g'

bash -c '
#echo bash=$SHELLOPTS
set -o | grep braceexpand | sed "$1"
' unused "$normalize"

env SHELLOPTS= bash -c '
#echo bash2=$SHELLOPTS
set -o | grep braceexpand | sed "$1"
' unused "$normalize"
