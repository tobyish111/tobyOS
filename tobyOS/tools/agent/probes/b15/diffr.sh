# diffr.sh: diff on two directories -- the native one against GNU diffutils
# (WSL) on the same trees: names in either only, subdirectories with and
# without -r, a directory against a file, identical and differing files,
# a link loop (`up -> ..`), and the header with the options as given.
exec 2>&1
D=${DIFFBIN:-diff}
cd /tmp; rm -rf dr1 dr2; mkdir -p dr1/sub dr2/sub dr1/onlya dr2/x
echo a > dr1/sub/f; echo b > dr2/sub/f; echo same > dr1/s; echo same > dr2/s
echo z > dr1/zz; echo y > dr2/yy; echo q > dr1/x; : > dr2/e; mkdir dr1/e
echo "== plain"; $D dr1 dr2; echo "rc=$?"
echo "== -r"; $D -r dr1 dr2; echo "rc=$?"
echo "== -ru"; $D -ru dr1 dr2 | sed 's/\t.*//'; echo "rc=$?"
echo "== -r -q"; $D -r -q dr1 dr2; echo "rc=$?"
echo "== -rs"; $D -rs dr1/sub dr2/sub; $D -rs dr1/sub dr1/sub; echo "rc=$?"
ln -s .. dr1/sub/up; ln -s .. dr2/sub/up
echo "== loop"; $D -r dr1 dr2 2>&1 | sort; echo "rc=$?"
echo "== loop rc"; $D -r dr1 dr2 > /dev/null 2>&1; echo "rc=$?"
rm -rf dr1 dr2
echo end
