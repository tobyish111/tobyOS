# dotdot.sh: `..` after a symbolic link is the PHYSICAL parent -- the
# target's -- for every path the kernel resolves (Linux's namei), and a
# missing or non-directory component before `..` is ENOENT or ENOTDIR, not
# skipped (finding 88's remainder). The shell's own `cd` is logical and is
# not asked here, except to set the directory the others start from.
exec 2>&1
cd /tmp; rm -rf ddt; mkdir -p ddt/a/b ddt/t/x
echo in-t > ddt/t/f; echo in-a > ddt/a/f; echo in-x > ddt/t/x/g
ln -s ../t/x ddt/a/lx; ln -s /tmp/ddt/t/x ddt/a/ax; ln -s lx ddt/a/lx2
r() { "$@" 2>&1 | sed 's/^[^:]*: //'; echo "  rc=${PIPESTATUS[0]}"; }
echo "== d1 cat a/lx/../f";      r cat ddt/a/lx/../f
echo "== d2 cat a/ax/../f";      r cat ddt/a/ax/../f
echo "== d3 cat a/lx2/../f";     r cat ddt/a/lx2/../f
echo "== d4 ls a/lx/..";         ls ddt/a/lx/.. | sort | tr '\n' ' '; echo
echo "== d5 cat a/lx/../../a/f"; r cat ddt/a/lx/../../a/f
echo "== d6 cat a/nosuch/../f";  r cat ddt/a/nosuch/../f
echo "== d7 cat a/f/../f";       r cat ddt/a/f/../f
echo "== d8 cat a/b/../f";       r cat ddt/a/b/../f
echo "== d9 [ -f a/lx/../f ]";   [ -f ddt/a/lx/../f ] && echo yes || echo no
echo "== d10 cat a/lx/x/..";     r cat ddt/a/lx/x/..
echo "== d11 from inside";       (cd -P ddt/a/lx && cat ../f)
echo "== d12 mkdir through";     mkdir ddt/a/lx/../new && ls ddt/t | sort | tr '\n' ' '; echo
echo "== d13 cp through";        cp ddt/a/f ddt/a/lx/../copied && cat ddt/t/copied
echo "== d14 /.. at the root";   r cat /../tmp/ddt/t/f
echo "== d15 busybox";           busybox cat ddt/a/lx/../f 2>&1
# the shell's own paths: test operands, a redirection's target, `.`, cd
echo only > ddt/t/onlyt; echo 'echo sourced' > ddt/t/s.sh
echo "== d16 test";   [ -e ddt/a/lx/../onlyt ] && echo yes || echo no; [ -e ddt/a/lx/../b ] && echo yes || echo no
echo "== d17 redirect"; echo red > ddt/a/lx/../red && cat ddt/t/red
echo "== d18 test from a linked cwd"; (cd ddt/a/lx && { [ -e ../onlyt ] && echo yes || echo no; })
echo "== d19 source"; . ddt/a/lx/../s.sh
echo "== d20 cd logical"; (cd ddt/a/lx/.. && pwd)
echo "== d21 cd -P"; (cd -P ddt/a/lx/.. && pwd)
echo "== d22 a missing dir's .."; echo x > ddt/a/nosuch/../w; echo "  rc=$?"; [ -e ddt/a/w ] && echo made || echo not-made
cd /tmp; rm -rf ddt
echo end
