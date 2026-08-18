shopt -s extglob
mkdir -p eg4
touch eg4/a 'eg4/a b' eg4/foo
argv.py eg4/@(a b|foo)
