exec 2>&1 </dev/null
# FILE CREATION MODES: a new file is open's mode less the umask, a new
# directory mkdir's mode less the umask (Linux: 0666 and 0777 from the
# shell and the tools)
d=/data/r2.$$
rm -rf "$d"; mkdir -p "$d" && cd "$d" || exit 1
echo "umask=$(umask)"
m() { stat -c '%n %a' "$@" | sed "s#$d/##"; }
for u in 000 022 077 027 002; do
  umask $u
  : > f$u; echo x >> a$u; mkdir d$u; touch t$u
  m f$u a$u d$u t$u
done
umask 022
# an existing file keeps its mode through > and >>
umask 000; : > keep; chmod 600 keep; echo y > keep; echo z >> keep; m keep
umask 022
# a 0444 file is not writable through > by its owner's process if not root,
# but root writes it; the test is the mode that stays
: > ro; chmod 444 ro; m ro
# mkdir -m, install -m, cp keep what they are told
mkdir -m 700 dm; m dm
umask 077; cp ro cpy; m cpy; umask 022
echo end
cd /; rm -rf "$d"
