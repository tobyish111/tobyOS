shopt -s extglob

mkdir -p extpipe
cd extpipe

touch '__|' foo
argv.py @('foo'|__\||bar)
argv.py @('foo'|'__|'|bar)
