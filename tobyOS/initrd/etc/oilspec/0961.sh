#command -v grep ls

command -v grep | egrep -o '/[^/]+$'
command -v ls | egrep -o '/[^/]+$'
echo

command -v true
command -v eval
