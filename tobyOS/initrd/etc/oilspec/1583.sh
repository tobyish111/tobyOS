shopt -s extglob
mkdir -p 1
cd 1
touch {foo,bar}.cc {foo,bar,baz}.h foo. foo.hh
ext=cc
echo foo.?($ext|h)
