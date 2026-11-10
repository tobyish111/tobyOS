exec 2>&1
cd /tmp; rm -rf gr; mkdir gr; cd gr
mkdir x; touch 'x/test.ifs.\.txt' 'x/test.ifs.*.txt'
v='*\*.txt'; echo 1 x/$v
v="\\" u='*.txt'; echo 2 x/*$v$u
v="\\" u="*.txt"; echo 3 x/*$v*.txt
shopt -s extglob
touch {foo,bar}.cc {foo,bar,baz}.h foo. foo.hh
ext=cc; echo 4 foo.?($ext|h)
echo 5 foo.+($ext|h)
a=(x); echo 6 @(fo|ba)*.$ext
cd /tmp; rm -rf gr
