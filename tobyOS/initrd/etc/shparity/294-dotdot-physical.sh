# `..` after a symbolic link is the PHYSICAL parent -- the target's -- for
# every path the kernel resolves and for the shell's own (test operands, a
# redirection's target, `.`), and a missing component before `..` fails;
# only a logical `cd` takes `..` by the text. Both took it lexically: `cat
# a/lx/../f` read a/f where bash reads the target's sibling (logs/b16
# dotdot).
exec 2>&1
cd /tmp; rm -rf sp294; mkdir -p sp294/a/b sp294/t/x
echo in-t > sp294/t/f; echo in-a > sp294/a/f; echo only > sp294/t/onlyt
echo 'echo sourced' > sp294/t/s.sh
ln -s ../t/x sp294/a/lx; ln -s /tmp/sp294/t/x sp294/a/ax; ln -s lx sp294/a/lx2
cat sp294/a/lx/../f sp294/a/ax/../f sp294/a/lx2/../f sp294/a/lx/../../a/f
ls sp294/a/lx/.. | sort | tr '\n' ' '; echo
cat sp294/a/nosuch/../f 2>/dev/null; echo "missing rc=$?"
cat sp294/a/f/../f 2>/dev/null; echo "notdir rc=$?"
cat sp294/a/b/../f
[ -e sp294/a/lx/../onlyt ] && echo t1-yes || echo t1-no
[ -e sp294/a/lx/../b ] && echo t2-yes || echo t2-no
[ -f sp294/a/lx/../f ] && echo t3-yes || echo t3-no
echo red > sp294/a/lx/../red && cat sp294/t/red
(cd sp294/a/lx && { [ -e ../onlyt ] && echo t4-yes || echo t4-no; cat ../f; })
. sp294/a/lx/../s.sh
(cd sp294/a/lx/.. && pwd)
(cd -P sp294/a/lx/.. && pwd)
mkdir sp294/a/lx/../new && ls sp294/t | sort | tr '\n' ' '; echo
{ echo x > sp294/a/nosuch/../w; } 2>/dev/null; echo "w rc=$?"
[ -e sp294/a/w ] && echo made || echo not-made
cd /tmp; rm -rf sp294
echo end
