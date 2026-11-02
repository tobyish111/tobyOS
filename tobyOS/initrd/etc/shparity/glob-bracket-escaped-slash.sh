# A slash escaped or quoted inside a bracket keeps the bracket a pattern --
# one no file name matches, so nullglob takes the word away and failglob
# fails it -- where a bare slash ends the bracket and the word is a name.
# tsh took the escaped slash for a bare one (finding 578).
exec 2>&1
cd "$(mktemp -d)" || exit 1
touch q w e
echo 1: [qwe/qwe] [qwe\/qwe] [qwe"/"] x['/']y
shopt -s nullglob
echo 2: [qwe/qwe] [qwe\/qwe] [qwe\/] [qwe"/"qwe] x['/']y
case / in [\/]) echo m1;; esac; [[ a/ == a[\/] ]] && echo m2
shopt -u nullglob; shopt -s failglob
echo 3: [qwe\/qwe]; echo "rc=$?"
echo 4: [qwe/qwe]; echo "rc=$?"
