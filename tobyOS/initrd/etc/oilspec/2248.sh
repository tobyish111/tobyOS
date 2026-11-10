case $SH in zsh) exit ;; esac

echo 'echo foo' > foo.sh

$SH -x -v -- foo.sh

echo -  
echo - >& 2

$SH -x -v - foo.sh


# I think it turns off -x -v with -
echo foo
+ echo foo
-

# set -o verbose not implemented for now
