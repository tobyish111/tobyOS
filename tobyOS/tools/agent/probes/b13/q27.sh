S=${SH:-bash}; S=$(command -v $S)
exec </dev/null
printf 'echo "script \$0=$0 \$#=$# \$1=${1-} \$-=$-"\n' > /tmp/q27s.sh
echo "== 1"; $S -e -c 'false; echo no'; echo rc=$?
echo "== 2"; $S -ec 'echo "$-"'
echo "== 3"; $S -xc 'echo hi' 2>&1
echo "== 4"; $S -o pipefail -c 'false | true; echo rc=$?'
echo "== 5"; $S +o hashall -c 'echo "$-"'
echo "== 6"; $S -O extglob -c 'shopt extglob'
echo "== 7"; $S -o nosuch -c 'echo x' 2>&1; echo rc=$?
echo "== 8"; $S -O nosuch -c 'echo x' 2>&1; echo rc=$?
echo "== 9"; $S -z -c 'echo x' 2>&1 | head -3; echo rc=$?
echo "== 10"; $S --nosuch -c 'echo x' 2>&1 | head -3
echo "== 11"; $S -c 2>&1; echo rc=$?
echo "== 12"; $S -c 'echo "$0 $# $1"' n1 a1 a2
echo "== 13"; $S /tmp/q27s.sh p1 p2
echo "== 14"; echo 'echo "stdin $0 $# $1 $-"' | $S -s q1 q2
echo "== 15"; echo 'echo "stdin2 $-"' | $S
echo "== 16"; $S -n -c 'echo should-not-run'; echo rc=$?
echo "== 17"; $S -- /tmp/q27s.sh x
echo "== 18"; $S - /tmp/q27s.sh y 2>&1 | head -2
echo "== 19"; $S -v -c 'echo v' 2>&1
echo "== 20"; printf 'echo from-bash-env\n' > /tmp/q27e.sh; BASH_ENV=/tmp/q27e.sh $S -c 'echo main'
echo "== 21"; X=/tmp; BASH_ENV='$X/q27e.sh' $S -c 'echo main2'
echo "== 22"; BASH_ENV=/tmp/q27e.sh $S /tmp/q27s.sh
echo "== 23"; ENV=/tmp/q27e.sh $S -c 'echo main3'
echo "== 24"; $S --norc --noprofile -c 'echo np'
echo "== 25"; $S --rcfile 2>&1 | head -2
echo "== 26"; $S --posix -c 'shopt -o posix'
echo "== 27"; $S -l -c 'shopt login_shell'
echo "== 28"; $S --login -c 'shopt login_shell'
echo "== 30"; $S --version | head -1 | cut -c1-9
echo "== 31"; $S -O extglob -O nullglob -c 'shopt extglob nullglob'
echo "== 32"; $S -abc 'echo "$-"'
echo "== 33"; $S +B -c 'echo {x,y}'
echo "== 34"; $S -o 2>&1 | head -2
echo "== 35"; $S -c 'echo $-' -x 2>&1
echo end
