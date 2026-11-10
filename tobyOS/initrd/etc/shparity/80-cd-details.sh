# POSIX cd: resolution via CDPATH PRINTS the directory it chose, `cd -`
# prints where it returned to, and a failed cd leaves the shell alive.
# The two shells run in DIFFERENT scratch dirs (<scratch>/a vs /b), so any
# printed path has the case's own $PWD prefix stripped first.
base=$PWD
mkdir -p box/sub inner
out=$(CDPATH=$base/box cd sub)
echo "1=${out#"$base"}"
cd inner
cd - >/dev/null
echo "2=[${PWD#"$base"}]"
cd nosuchdir
echo "3=$?"
( CDPATH=nope cd missing )
echo "4=$?"
echo end
