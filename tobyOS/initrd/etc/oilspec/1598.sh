shopt -s extglob
mkdir -p 3
cd 3

x='a b'
touch bar.{cc,h}

# OSH may disallow splitting when there's an extended glob
argv.py $x*.@(cc|h)
